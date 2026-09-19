/**
 * @file include/bkmail/imap/search_criteria.h
 * @brief IMAP SEARCH criteria carrier.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_SEARCH_CRITERIA_H_
#define BKMAIL_IMAP_SEARCH_CRITERIA_H_

#include <bkmail/common/detail/allocator_ext.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace bkmail::imap {

/**
 * @brief Owning, validated carrier for an RFC 3501 SEARCH criteria string
 * (e.g. `UNSEEN FROM "bob"`).
 *
 * Validation is structural, not grammatical: the text must be non-empty,
 * contain no CR/LF, and have balanced parentheses and double quotes
 * (honoring the `\"` and `\\` escapes). The server remains the authority
 * on the full SEARCH grammar.
 */
template <class Allocator = std::allocator<std::byte>>
class search_criteria {
 public:
  using allocator_type = Allocator;
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates an empty (invalid-to-send) criteria carrier.
  search_criteria() = default;

  /**
   * @brief Validates and stores `text`.
   * @throw std::invalid_argument when `text` fails structural validation.
   */
  explicit search_criteria(std::string_view text,
                           const Allocator& alloc = Allocator{})
      : text_(text, bkmail::detail::rebind_alloc_t<Allocator, char>(alloc)) {
    if (!is_valid(text_)) {
      throw std::invalid_argument("invalid IMAP search criteria");
    }
  }

  /// Non-throwing alternative to the constructors: returns `std::nullopt`
  /// when `text` fails structural validation.
  [[nodiscard]] static std::optional<search_criteria> parse(
      std::string_view text, const Allocator& alloc = Allocator{}) {
    if (!is_valid(text)) return std::nullopt;
    return search_criteria(text, alloc, unchecked_t{});
  }

  /// Structural validation: non-empty, no CR/LF, balanced parentheses and
  /// double quotes.
  [[nodiscard]] static bool is_valid(std::string_view text) noexcept {
    if (text.empty()) return false;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char c : text) {
      if (c == '\r' || c == '\n') return false;
      if (quoted) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          quoted = false;
        }
        continue;
      }
      if (c == '"') {
        quoted = true;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')') {
        if (--depth < 0) return false;
      }
    }
    return !quoted && depth == 0;
  }

  /// The validated criteria text, ready for the wire.
  [[nodiscard]] const string_type& str() const noexcept { return text_; }

  /// Returns true when the carrier holds no text.
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

  /// The allocator the stored text uses.
  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return allocator_type(text_.get_allocator());
  }

 private:
  /// Tag selecting the pre-validated constructor.
  struct unchecked_t {};

  search_criteria(std::string_view text, const Allocator& alloc, unchecked_t)
      : text_(text, bkmail::detail::rebind_alloc_t<Allocator, char>(alloc)) {}

  string_type text_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_SEARCH_CRITERIA_H_
