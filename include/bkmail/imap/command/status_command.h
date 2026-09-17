/**
 * @file include/bkmail/imap/command/status_command.h
 * @brief STATUS command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_STATUS_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_STATUS_COMMAND_H_

#include <bkmail/error.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/mailbox_status.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// STATUS command.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// The requested `status_items` are rendered as the parenthesized item
/// list (`MESSAGES RECENT UIDNEXT UIDVALIDITY UNSEEN`). Every
/// `* STATUS <mailbox> (<pairs>)` untagged response is parsed here (the
/// attribute table is the command's own deep parse); the last row seen
/// wins. The mailbox argument is rendered as an astring (atom when safe,
/// quoted otherwise; only `\"` and `\\` are escaped).
template <class Allocator = std::allocator<std::byte>>
class status_command {
 public:
  using allocator_type = Allocator;
  using result_type = mailbox_status<Allocator>;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  status_command(std::string_view mailbox, status_items items,
                 const Allocator& alloc = Allocator{})
      : alloc_(alloc), mailbox_(mailbox, alloc), items_(items), wire_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" STATUS ");
    append_arg(mailbox_);
    wire_.append(" (");
    append_items();
    wire_.append(")\r\n");
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    return emitted_ ? bnio::const_buffer{}
                    : bnio::const_buffer{wire_.data(), wire_.size()};
  }

  /// STATUS never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept { emitted_ = true; }

  /// STATUS expects no continuation; ignored.
  void on_continuation(std::string_view /*text*/) {}

  /// Parses each `* STATUS ...` attribute table (the last row wins).
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind != untagged_kind::status) return;
    // Data kinds carry their remainder in `payload` (a dispatch-lifetime
    // view); `text` is only set for the status kinds.
    parse_row(r.payload);
  }

  /// Advisory for the write pump: STATUS pipelines freely.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  /// FIFO ownership (operation_base.h): the `* STATUS ...` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::status;
  }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply; on OK the parsed status is moved into @p out.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    auto ec = status_ec(r.status);
    if (ec) return ec;
    out = status_;
    return {};
  }

 private:
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

  /// ASCII case-insensitive comparison of two views.
  static bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      char ca = a[i];
      char cb = b[i];
      if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
      if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
      if (ca != cb) return false;
    }
    return true;
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
    wire_.push_back('"');
    for (const char c : v) {
      if (c == '"' || c == '\\') wire_.push_back('\\');
      wire_.push_back(c);
    }
    wire_.push_back('"');
  }

  /// Appends the requested item names, space separated.
  void append_items() {
    bool first = true;
    const auto emit = [this, &first](bool on, std::string_view name) {
      if (!on) return;
      if (!first) wire_.push_back(' ');
      wire_.append(name);
      first = false;
    };
    emit(has_status_item(items_, status_items::messages), "MESSAGES");
    emit(has_status_item(items_, status_items::recent), "RECENT");
    emit(has_status_item(items_, status_items::uid_next), "UIDNEXT");
    emit(has_status_item(items_, status_items::uid_validity), "UIDVALIDITY");
    emit(has_status_item(items_, status_items::unseen), "UNSEEN");
  }

  /// Advances @p pos past any spaces.
  static void skip_sp(std::string_view text, std::size_t& pos) noexcept {
    while (pos < text.size() && text[pos] == ' ') ++pos;
  }

  /// Parses a quoted string at @p pos and discards its content.
  static void skip_quoted(std::string_view text, std::size_t& pos) noexcept {
    ++pos;  // opening quote
    while (pos < text.size()) {
      const char c = text[pos];
      if (c == '"') {
        ++pos;
        break;
      }
      pos += (c == '\\' && pos + 1 < text.size()) ? 2 : 1;
    }
  }

  /// Parses a `{n}\r\n<content>` literal at @p pos and discards it.
  static void skip_literal(std::string_view text, std::size_t& pos) noexcept {
    const auto close = text.find('}', pos);
    if (close == std::string_view::npos) {
      pos = text.size();
      return;
    }
    std::uint64_t n = 0;
    const auto res =
        std::from_chars(text.data() + pos + 1, text.data() + close, n);
    if (res.ec != std::errc{}) {
      pos = text.size();
      return;
    }
    pos = close + 1;
    if (text.substr(pos, 2) == "\r\n") pos += 2;
    const auto avail = text.size() - pos;
    pos += n < avail ? static_cast<std::size_t>(n) : avail;
  }

  /// Advances @p pos past the mailbox name (atom, quoted, or literal).
  static void skip_string(std::string_view text, std::size_t& pos) noexcept {
    if (pos >= text.size()) return;
    if (text[pos] == '"') {
      skip_quoted(text, pos);
      return;
    }
    if (text[pos] == '{') {
      skip_literal(text, pos);
      return;
    }
    while (pos < text.size() && text[pos] != ' ' && text[pos] != '(') ++pos;
  }

  /// Stores one `NAME number` pair into the status accumulator.
  void apply_pair(std::string_view name, std::string_view number) {
    std::uint32_t value = 0;
    const auto* first = number.data();
    const auto* last = number.data() + number.size();
    if (std::from_chars(first, last, value).ec != std::errc{}) return;
    if (iequals(name, "MESSAGES")) {
      status_.messages = value;
    } else if (iequals(name, "RECENT")) {
      status_.recent = value;
    } else if (iequals(name, "UIDNEXT")) {
      status_.uid_next = value;
    } else if (iequals(name, "UIDVALIDITY")) {
      status_.uid_validity = value;
    } else if (iequals(name, "UNSEEN")) {
      status_.unseen = value;
    }
    // Unknown items are ignored.
  }

  /// Deep-parses one `<mailbox> (<pairs>)` STATUS row.
  void parse_row(std::string_view text) {
    std::size_t pos = 0;
    skip_string(text, pos);  // mailbox name: parsed, not needed here.
    skip_sp(text, pos);
    if (pos >= text.size() || text[pos] != '(') return;
    const auto close = text.find(')', pos);
    if (close == std::string_view::npos) return;
    std::string_view body = text.substr(pos + 1, close - pos - 1);

    status_ = mailbox_status<Allocator>{};  // Last row wins: reset first.
    while (!body.empty()) {
      const auto sp = body.find(' ');
      if (sp == std::string_view::npos) break;  // name without number
      const std::string_view name = body.substr(0, sp);
      body.remove_prefix(sp + 1);
      const auto sp2 = body.find(' ');
      apply_pair(name, body.substr(0, sp2));
      if (sp2 == std::string_view::npos) break;
      body.remove_prefix(sp2 + 1);
    }
  }

  Allocator alloc_;
  string_type mailbox_;
  status_items items_;
  string_type wire_;  // Whole command line, tag included.
  bool emitted_ = false;
  mailbox_status<Allocator> status_{};  // Accumulated STATUS row.
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_STATUS_COMMAND_H_
