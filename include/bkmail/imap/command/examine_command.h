/**
 * @file include/bkmail/imap/command/examine_command.h
 * @brief EXAMINE command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_EXAMINE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_EXAMINE_COMMAND_H_

#include <bkmail/common/error.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace bkmail::imap {

/// EXAMINE command.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// EXAMINE is the read-only twin of SELECT: it assembles the same
/// `mailbox_info` snapshot from the untagged responses (`* n EXISTS`,
/// `* n RECENT`, `* FLAGS (...)`, and the `[UIDVALIDITY n]` /
/// `[UIDNEXT n]` / `[UNSEEN n]` / `[PERMANENTFLAGS (...)]` resp-codes of
/// untagged `OK`s), but `read_only` defaults to true. A tagged `NO`
/// leaves the mailbox unselected: it maps to `errc::command_rejected` and
/// the result stays default-constructed.
///
/// The mailbox argument is rendered as an astring (atom when safe, quoted
/// otherwise; only `\"` and `\\` are escaped).
template <class Allocator = std::allocator<std::byte>>
class examine_command {
 public:
  using allocator_type = Allocator;
  using result_type = mailbox_info<Allocator>;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  explicit examine_command(std::string_view mailbox,
                           const Allocator& alloc = Allocator{})
      : mailbox_(mailbox, alloc), wire_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" EXAMINE ");
    append_arg(mailbox_);
    wire_.append("\r\n");
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    return emitted_ ? bnio::const_buffer{}
                    : bnio::const_buffer{wire_.data(), wire_.size()};
  }

  /// EXAMINE never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept { emitted_ = true; }

  /// EXAMINE expects no continuation; ignored.
  void on_continuation(std::string_view /*text*/) {}

  /// Absorbs the EXAMINE untagged set into the snapshot accumulators.
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind == untagged_kind::exists) {
      exists_ = static_cast<std::uint32_t>(r.number);
    } else if (r.kind == untagged_kind::recent) {
      recent_ = static_cast<std::uint32_t>(r.number);
    } else if (r.kind == untagged_kind::flags) {
      // Data kinds carry their remainder in `payload` (a dispatch-lifetime
      // view); `text` is only set for the status kinds.
      parse_flag_list(r.payload, flags_);
    } else if (r.kind == untagged_kind::ok) {
      apply_ok_code(r.code);
    }
  }

  /// Advisory for the write pump: EXAMINE pipelines freely.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  /// FIFO ownership (operation_base.h): the `* FLAGS (...)` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::flags;
  }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply; on OK the assembled snapshot is moved into
  /// @p out (which the caller default-constructed).
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    auto ec = status_ec(r.status);
    if (ec) return ec;  // NO/BAD: mailbox stays unselected.

    // EXAMINE is read-only unless the server explicitly says READ-WRITE.
    const bool read_only = !(r.code.has_value() &&
                             std::holds_alternative<read_write_code>(*r.code));

    out.name = std::move(mailbox_);
    out.exists = exists_;
    out.recent = recent_;
    out.unseen = unseen_;
    out.uid_validity = uid_validity_;
    out.uid_next = uid_next_;
    out.flags = flags_;
    out.permanent_flags = permanent_flags_;
    out.read_only = read_only;
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

  /// Records one flag atom (system flags only; keywords are dropped).
  static void add_flag(std::string_view atom, flag_set& out) noexcept {
    if (atom.empty() || atom.front() != '\\') return;
    atom.remove_prefix(1);
    if (iequals(atom, "Seen")) {
      out.set(message_flag::seen);
    } else if (iequals(atom, "Answered")) {
      out.set(message_flag::answered);
    } else if (iequals(atom, "Flagged")) {
      out.set(message_flag::flagged);
    } else if (iequals(atom, "Deleted")) {
      out.set(message_flag::deleted);
    } else if (iequals(atom, "Draft")) {
      out.set(message_flag::draft);
    } else if (iequals(atom, "Recent")) {
      out.set(message_flag::recent);
    }
    // The PERMANENTFLAGS "\*" wildcard and unknown flags are dropped.
  }

  /// Parses a parenthesized flag list such as `(\Seen \Answered)`.
  static void parse_flag_list(std::string_view text, flag_set& out) {
    const auto open = text.find('(');
    if (open == std::string_view::npos) return;
    const auto close = text.find(')', open + 1);
    if (close == std::string_view::npos) return;
    std::string_view body = text.substr(open + 1, close - open - 1);
    while (!body.empty()) {
      const auto sp = body.find(' ');
      add_flag(body.substr(0, sp), out);
      if (sp == std::string_view::npos) break;
      body.remove_prefix(sp + 1);
    }
  }

  /// Applies the resp-code of an untagged OK to the snapshot.
  void apply_ok_code(const std::optional<response_code<Allocator>>& code) {
    if (!code.has_value()) return;
    if (const auto* v = std::get_if<uidvalidity_code>(&*code)) {
      uid_validity_ = v->value;
    } else if (const auto* v = std::get_if<uidnext_code>(&*code)) {
      uid_next_ = v->value;
    } else if (const auto* v = std::get_if<unseen_code>(&*code)) {
      unseen_ = v->value;
    } else if (const auto* v = std::get_if<permanentflags_code>(&*code)) {
      permanent_flags_ = v->flags;
    }
    // Other resp-codes (ALERT, PARSE, TRYCREATE, ...) are ignored.
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

  string_type mailbox_;  // Request argument; echoed into info.name.
  string_type wire_;     // Whole command line, tag included.
  bool emitted_ = false;

  // Snapshot accumulators (filled by on_untagged).
  std::uint32_t exists_ = 0;
  std::uint32_t recent_ = 0;
  std::optional<std::uint32_t> unseen_;
  std::uint32_t uid_validity_ = 0;
  std::uint32_t uid_next_ = 0;
  flag_set flags_;
  flag_set permanent_flags_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_EXAMINE_COMMAND_H_
