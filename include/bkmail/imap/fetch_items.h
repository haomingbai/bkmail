/**
 * @file include/bkmail/imap/fetch_items.h
 * @brief FETCH item selection and STORE mode.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_FETCH_ITEMS_H_
#define BKMAIL_IMAP_FETCH_ITEMS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bkmail::imap {

/// STORE flag-update mode: `+FLAGS` / `-FLAGS` / `FLAGS`.
enum class store_mode {
  add,
  remove,
  replace,
};

/**
 * @brief Selection of RFC 3501 FETCH data items.
 *
 * A bitmask over the item atoms (`ENVELOPE`, `BODYSTRUCTURE`, `FLAGS`,
 * `UID`, `INTERNALDATE`, `RFC822.SIZE`) plus a list of `BODY[...]` section
 * requests. Not allocator-templated: it is a small command parameter type,
 * rendered into command storage by the command layer.
 */
class fetch_items {
 public:
  /// The bitmask items.
  enum class item : std::uint32_t {
    envelope = 1u << 0,        ///< `ENVELOPE`
    body_structure = 1u << 1,  ///< `BODYSTRUCTURE`
    flags = 1u << 2,           ///< `FLAGS`
    uid = 1u << 3,             ///< `UID`
    internal_date = 1u << 4,   ///< `INTERNALDATE`
    rfc822_size = 1u << 5,     ///< `RFC822.SIZE`
  };

  /// One `BODY[specifier]` section request.
  struct section {
    /// Section specifier, e.g. `"HEADER.FIELDS (FROM TO)"`; empty for
    /// `BODY[]` (the whole message).
    std::string specifier;
    /// Render as `BODY.PEEK[...]` (no `\Seen` side effect) when true.
    bool peek = true;
  };

  /// Creates an empty selection.
  fetch_items() = default;

  /// Adds item `i` to the selection.
  fetch_items& add(item i) noexcept {
    bits_ |= static_cast<std::uint32_t>(i);
    return *this;
  }

  /// Removes item `i` from the selection.
  fetch_items& remove(item i) noexcept {
    bits_ &= ~static_cast<std::uint32_t>(i);
    return *this;
  }

  /// Returns true when item `i` is selected.
  [[nodiscard]] bool test(item i) const noexcept {
    return (bits_ & static_cast<std::uint32_t>(i)) != 0;
  }

  /// Adds a `BODY[specifier]` (or `BODY.PEEK[specifier]`) section request.
  fetch_items& add_section(std::string_view specifier, bool peek = true) {
    sections_.push_back(section{std::string(specifier), peek});
    return *this;
  }

  /// The requested body sections, in insertion order.
  [[nodiscard]] const std::vector<section>& sections() const noexcept {
    return sections_;
  }

  /// Returns true when at least one item or section is selected.
  [[nodiscard]] bool any() const noexcept {
    return bits_ != 0 || !sections_.empty();
  }

  /// Clears the selection.
  void clear() noexcept {
    bits_ = 0;
    sections_.clear();
  }

 private:
  std::uint32_t bits_ = 0;
  std::vector<section> sections_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_FETCH_ITEMS_H_
