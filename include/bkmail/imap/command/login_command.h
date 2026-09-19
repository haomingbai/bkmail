/**
 * @file include/bkmail/imap/command/login_command.h
 * @brief LOGIN command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_LOGIN_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_LOGIN_COMMAND_H_

#include <bkmail/common/account_info.h>
#include <bkmail/common/error.h>
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

/// LOGIN command.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// The user name and password are rendered as astrings: atom when safe,
/// quoted otherwise (only `\"` and `\\` are escaped), and as a literal
/// `{n}` when they contain CR/LF. A literal splits the command into
/// segments separated by continuation walls; with the server capability
/// LITERAL+ (pass `literal_plus = true`) `{n+}` is emitted instead and no
/// wall is raised.
template <class Allocator = std::allocator<std::byte>>
class login_command {
 public:
  using allocator_type = Allocator;
  using result_type = void;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  /// Stores the credentials. @p literal_plus selects non-synchronizing
  /// literals (`{n+}`, no continuation wait) when a literal is needed.
  explicit login_command(const account_info<Allocator>& account,
                         bool literal_plus = false,
                         const Allocator& alloc = Allocator{})
      : account_(account), literal_plus_(literal_plus), wire_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders every segment of the command.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" LOGIN ");
    std::size_t seg_begin = 0;
    append_arg(account_.user_name, seg_begin);
    wire_.push_back(' ');
    append_arg(account_.password, seg_begin);
    wire_.append("\r\n");
    close_segment(seg_begin);
    emitted_ = 0;
    unlocked_ = (walls_ == 0 || literal_plus_) ? seg_count_ : 1;
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (emitted_ >= unlocked_ || emitted_ >= seg_count_) return {};
    const auto [off, end] = seg_[emitted_];
    return bnio::const_buffer{wire_.data() + off, end - off};
  }

  /// True while a literal wall blocks the remaining segments.
  [[nodiscard]] bool awaits_continuation() const noexcept {
    return unlocked_ < seg_count_;
  }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept {
    if (emitted_ < seg_count_) ++emitted_;
  }

  /// Releases the segment behind the next literal wall.
  void on_continuation(std::string_view /*text*/) noexcept {
    if (unlocked_ < seg_count_) ++unlocked_;
  }

  /// LOGIN collects no untagged responses.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Advisory for the write pump: LOGIN pipelines once past its literals.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply to the handler error code (NO -> rejected,
  /// BAD -> bad_command).
  [[nodiscard]] std::error_code on_tagged(
      const tagged_response<Allocator>& r) const {
    return status_ec(r.status);
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

  /// Appends @p v in quoted form, escaping only `\"` and `\\`.
  void append_quoted(std::string_view v) {
    wire_.push_back('"');
    for (const char c : v) {
      if (c == '"' || c == '\\') wire_.push_back('\\');
      wire_.push_back(c);
    }
    wire_.push_back('"');
  }

  /// Closes the pending segment at the current write position.
  void close_segment(std::size_t& seg_begin) {
    if (seg_count_ < seg_.size()) {
      seg_[seg_count_++] = {seg_begin, wire_.size()};
    }
    seg_begin = wire_.size();
  }

  /// Appends one astring argument, splitting segments at literals.
  void append_arg(std::string_view v, std::size_t& seg_begin) {
    if (atom_safe(v)) {
      wire_.append(v);
      return;
    }
    if (v.find_first_of("\r\n") == std::string_view::npos) {
      append_quoted(v);
      return;
    }
    // Synchronizing (or LITERAL+) literal: the "{n}\r\n" line closes the
    // current segment and raises a wall; the content opens the next one.
    wire_.push_back('{');
    wire_.append(std::to_string(v.size()));
    if (literal_plus_) wire_.push_back('+');
    wire_.append("}\r\n");
    close_segment(seg_begin);
    ++walls_;
    wire_.append(v);
  }

  account_info<Allocator> account_;
  bool literal_plus_ = false;
  string_type wire_;  // All segments, concatenated.
  std::array<std::pair<std::size_t, std::size_t>, 4> seg_{};
  std::size_t seg_count_ = 0;
  std::size_t walls_ = 0;     // Literal walls placed while rendering.
  std::size_t emitted_ = 0;   // Segments handed to the write pump.
  std::size_t unlocked_ = 0;  // Segments past the last continuation wall.
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_LOGIN_COMMAND_H_
