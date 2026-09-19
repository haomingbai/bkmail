/**
 * @file include/bkmail/imap/command/list_command.h
 * @brief LIST command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_LIST_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_LIST_COMMAND_H_

#include <bkmail/common/error.h>
#include <bkmail/imap/mailbox_entry.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bkmail::imap {

/// LIST command.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// The reference and pattern arguments are rendered as astrings (atom when
/// safe, quoted otherwise; only `\"` and `\\` are escaped — `%` and `*`
/// keep their wildcard meaning inside the quoted form).
///
/// Every `* LIST (<flags>) <delimiter> <name>` untagged response is parsed
/// here (the deep parse of the row is the command's own job): `\Noselect`,
/// `\HasChildren` and `\HasNoChildren` map onto the `mailbox_entry`
/// booleans, other attributes are dropped; the delimiter may be a quoted
/// character or `NIL`; the name may arrive as an atom, a quoted string, or
/// a `{n}` literal (already reassembled by the response lexer).
template <class Allocator = std::allocator<std::byte>>
class list_command {
 public:
  using allocator_type = Allocator;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;
  using result_type =
      std::vector<mailbox_entry<Allocator>,
                  typename std::allocator_traits<Allocator>::
                      template rebind_alloc<mailbox_entry<Allocator>>>;

  list_command(std::string_view reference, std::string_view pattern,
               const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        reference_(reference, alloc),
        pattern_(pattern, alloc),
        wire_(alloc),
        entries_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" LIST ");
    append_arg(reference_);
    wire_.push_back(' ');
    append_arg(pattern_);
    wire_.append("\r\n");
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    return emitted_ ? bnio::const_buffer{}
                    : bnio::const_buffer{wire_.data(), wire_.size()};
  }

  /// LIST never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept { emitted_ = true; }

  /// LIST expects no continuation; ignored.
  void on_continuation(std::string_view /*text*/) {}

  /// Parses each `* LIST ...` row into the entry vector.
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind != untagged_kind::list) return;
    // Data kinds carry their remainder in `payload` (a dispatch-lifetime
    // view); `text` is only set for the status kinds.
    parse_row(r.payload);
  }

  /// Advisory for the write pump: LIST pipelines freely.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  /// FIFO ownership (operation_base.h): the `* LIST ...` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::list;
  }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply; on OK the parsed rows are moved into @p out.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    auto ec = status_ec(r.status);
    if (ec) return ec;
    out = std::move(entries_);
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

  /// Advances @p pos past any spaces.
  static void skip_sp(std::string_view text, std::size_t& pos) noexcept {
    while (pos < text.size() && text[pos] == ' ') ++pos;
  }

  /// Parses a quoted string at @p pos; returns its unescaped content.
  string_type parse_quoted(std::string_view text, std::size_t& pos) const {
    string_type out(alloc_);
    ++pos;  // opening quote
    while (pos < text.size()) {
      const char c = text[pos];
      if (c == '"') {
        ++pos;
        break;
      }
      if (c == '\\' && pos + 1 < text.size()) {
        out.push_back(text[pos + 1]);  // only \" and \\ are produced
        pos += 2;
        continue;
      }
      out.push_back(c);
      ++pos;
    }
    return out;
  }

  /// Parses a `{n}\r\n<content>` literal at @p pos (content already
  /// reassembled into the response text by the lexer).
  string_type parse_literal(std::string_view text, std::size_t& pos) const {
    string_type out(alloc_);
    const auto close = text.find('}', pos);
    if (close == std::string_view::npos) {
      pos = text.size();
      return out;
    }
    std::uint64_t n = 0;
    const auto* first = text.data() + pos + 1;
    const auto res = std::from_chars(first, text.data() + close, n);
    if (res.ec != std::errc{}) {
      pos = text.size();
      return out;
    }
    pos = close + 1;
    if (text.substr(pos, 2) == "\r\n") pos += 2;
    const auto avail = text.size() - pos;
    const auto take = n < avail ? static_cast<std::size_t>(n) : avail;
    out.append(text.substr(pos, take));
    pos += take;
    return out;
  }

  /// Parses an atom at @p pos (stops at SP / `(` / `)`).
  string_type parse_atom(std::string_view text, std::size_t& pos) const {
    const auto begin = pos;
    while (pos < text.size() && text[pos] != ' ' && text[pos] != '(' &&
           text[pos] != ')') {
      ++pos;
    }
    return string_type(text.substr(begin, pos - begin), alloc_);
  }

  /// Parses one string token in any of the three wire forms.
  string_type parse_string(std::string_view text, std::size_t& pos) const {
    if (pos >= text.size()) return string_type(alloc_);
    if (text[pos] == '"') return parse_quoted(text, pos);
    if (text[pos] == '{') return parse_literal(text, pos);
    return parse_atom(text, pos);
  }

  /// Records the mailbox-attribute atoms of one LIST row.
  static void apply_row_flags(std::string_view body,
                              mailbox_entry<Allocator>& entry) noexcept {
    while (!body.empty()) {
      const auto sp = body.find(' ');
      std::string_view attr = body.substr(0, sp);
      if (!attr.empty() && attr.front() == '\\') {
        attr.remove_prefix(1);
        if (iequals(attr, "Noselect")) {
          entry.no_select = true;
        } else if (iequals(attr, "HasChildren")) {
          entry.has_children = true;
        } else if (iequals(attr, "HasNoChildren")) {
          entry.has_no_children = true;
        }
        // \Marked, \Unmarked and extension attributes are dropped.
      }
      if (sp == std::string_view::npos) break;
      body.remove_prefix(sp + 1);
    }
  }

  /// Deep-parses one `(<flags>) <delimiter> <name>` LIST row.
  void parse_row(std::string_view text) {
    std::size_t pos = 0;
    if (pos >= text.size() || text[pos] != '(') return;
    const auto close = text.find(')', pos);
    if (close == std::string_view::npos) return;

    mailbox_entry<Allocator> entry{};
    apply_row_flags(text.substr(pos + 1, close - pos - 1), entry);
    pos = close + 1;

    skip_sp(text, pos);
    if (pos + 3 <= text.size() && iequals(text.substr(pos, 3), "NIL")) {
      pos += 3;  // No hierarchy delimiter: entry.delimiter stays empty.
    } else {
      entry.delimiter = parse_string(text, pos);
    }

    skip_sp(text, pos);
    entry.name = parse_string(text, pos);
    entries_.push_back(std::move(entry));
  }

  Allocator alloc_;
  string_type reference_;
  string_type pattern_;
  string_type wire_;  // Whole command line, tag included.
  bool emitted_ = false;
  result_type entries_;  // Accumulated LIST rows.
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_LIST_COMMAND_H_
