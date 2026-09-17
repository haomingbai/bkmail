/**
 * @file include/bkmail/imap/sequence_set.h
 * @brief Validated IMAP sequence-set value type.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_SEQUENCE_SET_H_
#define BKMAIL_IMAP_SEQUENCE_SET_H_

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bkmail::imap {

/**
 * @brief Validated, renderable RFC 3501 `seq-set` value (`2:4,7:*`).
 *
 * Values are 32-bit sequence numbers or UIDs; `*` is the largest number in
 * the mailbox. Validation follows the RFC 3501 grammar:
 * `seq-set = seq-range *("," seq-range)`,
 * `seq-range = seq-number [":" seq-number]`,
 * `seq-number = nz-number / "*"`, with numbers in `[1, 2^32-1]`.
 */
class sequence_set {
 public:
  /// Creates an empty (invalid-to-send) sequence set.
  sequence_set() = default;

  /**
   * @brief Validates and stores `text`.
   * @throw std::invalid_argument when `text` is not a valid seq-set.
   */
  explicit sequence_set(std::string_view text) : text_(text) {
    if (!is_valid(text_)) {
      throw std::invalid_argument("invalid IMAP sequence set");
    }
  }

  /// Non-throwing alternative to the constructor: returns `std::nullopt`
  /// when `text` is not a valid seq-set.
  [[nodiscard]] static std::optional<sequence_set> parse(
      std::string_view text) {
    if (!is_valid(text)) return std::nullopt;
    sequence_set result;
    result.text_ = text;
    return result;
  }

  /// Returns true when `text` is a valid RFC 3501 seq-set.
  [[nodiscard]] static bool is_valid(std::string_view text) noexcept {
    if (text.empty()) return false;
    std::size_t pos = 0;
    for (;;) {
      pos = parse_seq_number(text, pos);
      if (pos == std::string_view::npos) return false;
      if (pos < text.size() && text[pos] == ':') {
        pos = parse_seq_number(text, pos + 1);
        if (pos == std::string_view::npos) return false;
      }
      if (pos == text.size()) return true;
      if (text[pos] != ',') return false;
      ++pos;
      if (pos == text.size()) return false;  // trailing comma
    }
  }

  /// The validated seq-set text, ready for the wire.
  [[nodiscard]] const std::string& str() const noexcept { return text_; }

  /// Returns true when the set holds no text.
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

  friend bool operator==(const sequence_set&, const sequence_set&) = default;

 private:
  /// Parses one `seq-number` (`nz-number` within 32 bits, or `*`) at `pos`;
  /// returns the index just past it, or `npos` on failure.
  static std::size_t parse_seq_number(std::string_view text,
                                      std::size_t pos) noexcept {
    if (pos >= text.size()) return std::string_view::npos;
    if (text[pos] == '*') return pos + 1;
    if (text[pos] < '1' || text[pos] > '9') return std::string_view::npos;
    std::uint64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      value = value * 10 + static_cast<std::uint64_t>(text[pos] - '0');
      if (value > 0xFFFFFFFFull) return std::string_view::npos;
      ++pos;
    }
    return pos;
  }

  std::string text_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_SEQUENCE_SET_H_
