/**
 * @file include/bkmail/imap/detail/parse_cursor.h
 * @brief Non-owning deep-parse cursor over one complete IMAP response.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_PARSE_CURSOR_H_
#define BKMAIL_IMAP_DETAIL_PARSE_CURSOR_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/imap/detail/astring.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace bkmail::imap::detail {

/**
 * A non-owning cursor over one complete response, offered to the deep parsers
 * living in the command files (ENVELOPE, BODYSTRUCTURE, FETCH attributes,
 * ...). All accessors return views into the response text, so every result
 * dies with the response (dispatch lifetime); `read_quoted()` may
 * materialize an unescaped value into an internal scratch buffer, in which
 * case the view is invalidated by the next `read_quoted()` call.
 *
 * Literal handling: a literal appears inside the response text as
 * `{n}\r\n` followed by `n` raw octets; `read_literal()` / `read_string()` /
 * `read_astring()` / `read_nstring()` skip the marker and return a view of
 * exactly those `n` octets.
 */
template <class Allocator = std::allocator<std::byte>>
class parse_cursor {
 public:
  /// Allocator-aware string type used for the unescape scratch.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates a cursor over `text` (one complete response, CRLF stripped).
  explicit parse_cursor(std::string_view text,
                        const Allocator& alloc = Allocator{})
      : text_(text), scratch_(alloc) {}

  /// Returns true when no input remains.
  [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

  /// Returns the current character, or `'\0'` at end of input.
  [[nodiscard]] char peek() const noexcept {
    return at_end() ? '\0' : text_[pos_];
  }

  /// Current offset into the response text.
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

  /// View of the unconsumed remainder.
  [[nodiscard]] std::string_view remaining() const noexcept {
    return text_.substr(pos_);
  }

  /// Consumes one character if it equals `expected`.
  bool try_consume(char expected) noexcept {
    if (!at_end() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  /// Consumes `word` (ASCII case-insensitive) if it prefixes the remainder.
  bool try_consume_ci(std::string_view word) noexcept {
    const std::string_view rest = remaining();
    if (rest.size() < word.size() ||
        !ascii_iequals(rest.substr(0, word.size()), word)) {
      return false;
    }
    pos_ += word.size();
    return true;
  }

  /// Skips a run of SP characters.
  void skip_spaces() noexcept {
    while (!at_end() && text_[pos_] == ' ') {
      ++pos_;
    }
  }

  /// Reads up to (not past) the first character contained in `stops`.
  [[nodiscard]] std::string_view read_until_any(
      std::string_view stops) noexcept {
    const std::size_t start = pos_;
    while (!at_end() && stops.find(text_[pos_]) == std::string_view::npos) {
      ++pos_;
    }
    return text_.substr(start, pos_ - start);
  }

  /// Reads one atom (stops at atom-specials, SP, CR, LF); may be empty.
  [[nodiscard]] std::string_view read_atom() noexcept {
    const std::size_t start = pos_;
    while (!at_end() && is_atom_char(text_[pos_])) {
      ++pos_;
    }
    return text_.substr(start, pos_ - start);
  }

  /// Reads an unsigned decimal number; `std::nullopt` when there are no
  /// digits or the value overflows 64 bits.
  [[nodiscard]] std::optional<std::uint64_t> read_number() noexcept {
    const std::size_t start = pos_;
    while (!at_end() && is_digit(text_[pos_])) {
      ++pos_;
    }
    if (pos_ == start) {
      return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto res =
        std::from_chars(text_.data() + start, text_.data() + pos_, value);
    if (res.ec != std::errc{}) {
      return std::nullopt;
    }
    return value;
  }

  /// Reads a quoted string and returns its unescaped content. Only `\"` and
  /// `\\` escapes are legal (RFC 3501 §9). `std::nullopt` on malformed input
  /// (unterminated, illegal escape, embedded CR/LF).
  [[nodiscard]] std::optional<std::string_view> read_quoted() {
    if (!try_consume('"')) {
      return std::nullopt;
    }
    const std::size_t start = pos_;
    bool escaped = false;
    while (!at_end() && text_[pos_] != '"') {
      const char c = text_[pos_];
      if (c == '\\') {
        escaped = true;
        ++pos_;
        if (at_end() || !is_quoted_special(text_[pos_])) {
          return std::nullopt;
        }
      } else if (c == '\r' || c == '\n') {
        return std::nullopt;
      }
      ++pos_;
    }
    if (at_end()) {
      return std::nullopt;  // Unterminated.
    }
    const std::string_view body = text_.substr(start, pos_ - start);
    ++pos_;  // Closing quote.
    if (!escaped) {
      return body;
    }
    scratch_.clear();
    for (std::size_t i = 0; i < body.size(); ++i) {
      if (body[i] == '\\') {
        ++i;  // Validated above; skip the backslash.
      }
      scratch_.push_back(body[i]);
    }
    return std::string_view(scratch_.data(), scratch_.size());
  }

  /// Reads a literal `{n}\r\n<n octets>` (`{n+}` and `~{n}` accepted) and
  /// returns a view of the raw octets. `std::nullopt` on malformed input.
  [[nodiscard]] std::optional<std::string_view> read_literal() noexcept {
    std::size_t p = pos_;
    const std::size_t size = text_.size();
    if (p < size && text_[p] == '~') {
      ++p;  // Obsolete LITERAL- spelling.
    }
    if (p >= size || text_[p] != '{') {
      return std::nullopt;
    }
    ++p;
    const std::size_t digits_start = p;
    while (p < size && is_digit(text_[p])) {
      ++p;
    }
    if (p == digits_start) {
      return std::nullopt;
    }
    std::uint64_t count = 0;
    const auto res =
        std::from_chars(text_.data() + digits_start, text_.data() + p, count);
    if (res.ec != std::errc{}) {
      return std::nullopt;
    }
    if (p < size && text_[p] == '+') {
      ++p;  // LITERAL+ marker.
    }
    if (p >= size || text_[p] != '}') {
      return std::nullopt;
    }
    ++p;
    if (p + 1 >= size || text_[p] != '\r' || text_[p + 1] != '\n') {
      return std::nullopt;
    }
    p += 2;
    if (static_cast<std::uint64_t>(size - p) < count) {
      return std::nullopt;  // The lexer guarantees presence; defensive.
    }
    pos_ = p + static_cast<std::size_t>(count);
    return text_.substr(p, static_cast<std::size_t>(count));
  }

  /// Reads a string (quoted or literal).
  [[nodiscard]] std::optional<std::string_view> read_string() {
    const char c = peek();
    if (c == '"') {
      return read_quoted();
    }
    if (c == '{' || c == '~') {
      return read_literal();
    }
    return std::nullopt;
  }

  /// Reads an astring (atom or string).
  [[nodiscard]] std::optional<std::string_view> read_astring() {
    const char c = peek();
    if (c == '"' || c == '{' || c == '~') {
      return read_string();
    }
    const std::string_view atom = read_atom();
    if (atom.empty()) {
      return std::nullopt;
    }
    return atom;
  }

  /// Reads an nstring (`NIL` / string). The outer optional is `std::nullopt`
  /// on malformed input; the inner one is `std::nullopt` for `NIL`.
  [[nodiscard]] std::optional<std::optional<std::string_view>> read_nstring() {
    const std::string_view rest = remaining();
    if (rest.size() >= 3 && ascii_iequals(rest.substr(0, 3), "NIL") &&
        (rest.size() == 3 || !is_atom_char(rest[3]))) {
      pos_ += 3;
      return {std::optional<std::string_view>{std::nullopt}};
    }
    if (const auto value = read_string()) {
      return {std::optional<std::string_view>{*value}};
    }
    return std::nullopt;
  }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
  string_type scratch_;  ///< Unescape scratch for `read_quoted`.
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_PARSE_CURSOR_H_
