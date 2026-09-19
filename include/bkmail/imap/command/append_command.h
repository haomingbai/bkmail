/**
 * @file include/bkmail/imap/command/append_command.h
 * @brief APPEND command with literal continuation.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_APPEND_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_APPEND_COMMAND_H_

#include <bkmail/common/error.h>
#include <bkmail/common/mail.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace bkmail::imap {

/// APPEND command with literal continuation.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// The message is always sent as a literal, so the command runs the
/// literal handshake: segment 0 ends with the `{n}\r\n` line and the
/// command waits for the continuation request before segment 1 (the
/// message bytes plus the terminating CRLF) may be staged. When the
/// server advertises LITERAL+ (pass `literal_plus = true`) `{n+}` is
/// emitted instead and no continuation wait is raised. With UIDPLUS the
/// tagged OK carries an `[APPENDUID uidvalidity uid]` resp-code; the
/// command result is `void`, so the code is consumed but not surfaced
/// (callers needing it can issue `raw_command`).
///
/// The mailbox argument is rendered as an astring (atom when safe, quoted
/// otherwise; only `\"` and `\\` are escaped). The flag list is omitted
/// when empty; the date-time is emitted as a quoted string when given.
template <class Allocator = std::allocator<std::byte>>
class append_command {
 public:
  using allocator_type = Allocator;
  using result_type = void;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  /// Stores the arguments. @p message is the raw RFC 5322 octet stream;
  /// @p date_time, when non-empty, is an IMAP date-time string
  /// (`"17-Jul-1996 02:44:25 -0700"`). @p literal_plus mirrors the
  /// server's LITERAL+ capability.
  append_command(std::string_view mailbox, std::string_view message,
                 flag_set flags = flag_set{}, std::string_view date_time = {},
                 bool literal_plus = false,
                 const Allocator& alloc = Allocator{})
      : mailbox_(mailbox, alloc),
        message_(message, alloc),
        flags_(flags),
        date_time_(date_time, alloc),
        literal_plus_(literal_plus),
        wire_(alloc) {}

  /// Stores the arguments, rendering @p message into its RFC 5322 octet
  /// stream first (well-known header fields plus the body octets; no MIME
  /// re-encoding). This is the docs/usage.md call shape
  /// (`append_command{mailbox, const mail&, flag_set}`).
  append_command(std::string_view mailbox, const mail<Allocator>& message,
                 flag_set flags = flag_set{}, std::string_view date_time = {},
                 bool literal_plus = false,
                 const Allocator& alloc = Allocator{})
      : append_command(mailbox, std::string_view{render_mail(message, alloc)},
                       flags, date_time, literal_plus, alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders both segments of the command.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" APPEND ");
    append_arg(mailbox_);
    if (flags_.any()) append_flags();
    if (!date_time_.empty()) {
      wire_.push_back(' ');
      append_quoted(date_time_);
    }
    wire_.push_back(' ');
    wire_.push_back('{');
    wire_.append(std::to_string(message_.size()));
    if (literal_plus_) wire_.push_back('+');
    wire_.append("}\r\n");
    seg_[0] = {0, wire_.size()};
    const std::size_t seg_begin = wire_.size();
    wire_.append(message_);
    wire_.append("\r\n");
    seg_[1] = {seg_begin, wire_.size()};
    seg_count_ = 2;
    emitted_ = 0;
    unlocked_ = literal_plus_ ? 2 : 1;
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (emitted_ >= unlocked_ || emitted_ >= seg_count_) return {};
    const auto [off, end] = seg_[emitted_];
    return bnio::const_buffer{wire_.data() + off, end - off};
  }

  /// True while the literal wall blocks the message segment.
  [[nodiscard]] bool awaits_continuation() const noexcept {
    return unlocked_ < seg_count_;
  }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept {
    if (emitted_ < seg_count_) ++emitted_;
  }

  /// Releases the message segment after the continuation request.
  void on_continuation(std::string_view /*text*/) noexcept {
    if (unlocked_ < seg_count_) ++unlocked_;
  }

  /// APPEND collects no untagged responses.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Advisory for the write pump: the literal wall already serializes.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply to the handler error code (NO -> rejected,
  /// BAD -> bad_command). An `[APPENDUID]` resp-code on OK is consumed
  /// and intentionally not surfaced (result_type is void).
  [[nodiscard]] std::error_code on_tagged(
      const tagged_response<Allocator>& r) const {
    return status_ec(r.status);
  }

 private:
  /// Renders a mail value into its RFC 5322 octet stream (best-effort:
  /// well-known header fields only, no MIME re-encoding).
  static string_type render_mail(const mail<Allocator>& message,
                                 const Allocator& alloc) {
    using address_list = typename mail_header<Allocator>::address_list;
    string_type out(alloc);
    const auto& h = message.header;
    const auto put_field = [&out](std::string_view name,
                                  std::string_view value) {
      if (value.empty()) return;
      out.append(name);
      out.append(": ");
      out.append(value);
      out.append("\r\n");
    };
    const auto put_addresses = [&out](std::string_view name,
                                      const address_list& list) {
      if (list.empty()) return;
      out.append(name);
      out.append(": ");
      bool first = true;
      for (const auto& addr : list) {
        if (!first) out.append(", ");
        first = false;
        const string_type email = addr.email();
        if (addr.display_name.empty()) {
          out.append(email);
        } else {
          out.push_back('"');
          out.append(addr.display_name);
          out.append("\" <");
          out.append(email);
          out.push_back('>');
        }
      }
      out.append("\r\n");
    };
    put_field("Subject", h.subject);
    put_addresses("From", h.from);
    put_addresses("To", h.to);
    put_addresses("Cc", h.cc);
    put_addresses("Bcc", h.bcc);
    put_field("Date", h.date);
    put_field("Message-ID", h.message_id);
    put_field("In-Reply-To", h.in_reply_to);
    put_field("Content-Type", message.body.content_type);
    out.append("\r\n");
    const auto& data = message.body.data;
    out.append(reinterpret_cast<const char*>(data.data()), data.size());
    return out;
  }

  /// Maps a tagged status onto the callback-path error code contract.
  static std::error_code status_ec(response_status status) {
    switch (status) {
      case response_status::ok:
        return {};
      case response_status::no:
        return make_error_code(errc::command_rejected);
      case response_status::bad:
        return make_error_code(errc::bad_command);
      default:
        return make_error_code(errc::unexpected_response);
    }
  }

  /// True when @p v can be sent as a bare atom (ASTRING-CHAR run).
  static bool atom_safe(std::string_view v) noexcept {
    if (v.empty()) return false;
    for (const char c : v) {
      const auto u = static_cast<unsigned char>(c);
      if (u < 0x20 || u >= 0x7f) return false;  // CTL / 8-bit
      switch (c) {
        case '(':
        case ')':
        case '{':
        case ' ':
        case '%':
        case '*':
        case '"':
        case '\\':
        case ']':
          return false;
        default:
          break;
      }
    }
    return true;
  }

  /// Appends one astring argument (atom when safe, quoted otherwise).
  void append_arg(std::string_view v) {
    if (atom_safe(v)) {
      wire_.append(v);
      return;
    }
    append_quoted(v);
  }

  /// Appends @p v in quoted form, escaping only `\"` and `\\`.
  void append_quoted(std::string_view v) {
    wire_.push_back('"');
    for (const char c : v) {
      if (c == '"' || c == '\\') wire_.push_back('\\');
      wire_.push_back(c);
    }
    wire_.push_back('"');
  }

  /// Appends the parenthesized flag list.
  void append_flags() {
    wire_.append(" (");
    bool first = true;
    const auto emit = [this, &first](bool on, std::string_view name) {
      if (!on) return;
      if (!first) wire_.push_back(' ');
      wire_.append(name);
      first = false;
    };
    emit(flags_.test(message_flag::answered), "\\Answered");
    emit(flags_.test(message_flag::deleted), "\\Deleted");
    emit(flags_.test(message_flag::draft), "\\Draft");
    emit(flags_.test(message_flag::flagged), "\\Flagged");
    emit(flags_.test(message_flag::recent), "\\Recent");
    emit(flags_.test(message_flag::seen), "\\Seen");
    wire_.push_back(')');
  }

  string_type mailbox_;
  string_type message_;  // Raw message octets.
  flag_set flags_;
  string_type date_time_;
  bool literal_plus_ = false;
  string_type wire_;  // Both segments, concatenated.
  std::array<std::pair<std::size_t, std::size_t>, 2> seg_{};
  std::size_t seg_count_ = 0;
  std::size_t emitted_ = 0;   // Segments handed to the write pump.
  std::size_t unlocked_ = 0;  // Segments past the literal wall.
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_APPEND_COMMAND_H_
