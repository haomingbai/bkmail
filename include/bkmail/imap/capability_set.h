/**
 * @file include/bkmail/imap/capability_set.h
 * @brief Parsed IMAP capability set with case-insensitive lookup.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_CAPABILITY_SET_H_
#define BKMAIL_IMAP_CAPABILITY_SET_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/export.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace bkmail::imap::detail {

/**
 * @brief ASCII case-insensitive equality for capability names.
 *
 * Compiled in `capabilities.cpp` so the folding logic is not inlined into
 * every TU that parses CAPABILITY lines.
 */
[[nodiscard]] BKMAIL_EXPORT bool capability_name_equals(
    std::string_view a, std::string_view b) noexcept;

/**
 * @brief Uppercases ASCII letters in `[data, data + size)` in place.
 *
 * Compiled in `capabilities.cpp`; used to normalize interned capability
 * names.
 */
BKMAIL_EXPORT void capability_name_to_upper(char* data,
                                            std::size_t size) noexcept;

}  // namespace bkmail::imap::detail

namespace bkmail::imap {

/**
 * @brief Parsed CAPABILITY/greeting capability set.
 *
 * Capabilities are interned uppercase-normalized; lookup is
 * case-insensitive, so `caps.contains("uidplus")` and
 * `caps.contains("UIDPLUS")` agree. Used to probe `LITERAL+`, `SASL-IR`,
 * `IDLE`, `UIDPLUS`, `STARTTLS`, ... availability.
 */
template <class Allocator = std::allocator<std::byte>>
class capability_set {
 public:
  using allocator_type = Allocator;
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Storage container of interned (uppercase-normalized) names.
  using container_type = bkmail::detail::vector_of<string_type, Allocator>;
  using const_iterator = typename container_type::const_iterator;

  /// Creates an empty set with a default-constructed allocator.
  capability_set() = default;

  /// Creates an empty set using `alloc`.
  explicit capability_set(const Allocator& alloc) : capabilities_(alloc) {}

  /**
   * @brief Interns `cap`, uppercase-normalized; duplicates (compared
   * case-insensitively) and empty names are ignored.
   */
  void insert(std::string_view cap) {
    if (cap.empty()) return;
    if (contains(cap)) return;
    string_type stored(bkmail::detail::rebind_alloc_t<Allocator, char>(
        capabilities_.get_allocator()));
    stored.append(cap.data(), cap.size());
    detail::capability_name_to_upper(stored.data(), stored.size());
    capabilities_.push_back(std::move(stored));
  }

  /// Case-insensitive membership test, e.g. `caps.contains("UIDPLUS")`.
  [[nodiscard]] bool contains(std::string_view name) const noexcept {
    for (const auto& stored : capabilities_) {
      if (detail::capability_name_equals(stored, name)) return true;
    }
    return false;
  }

  /// Returns true when the set is empty.
  [[nodiscard]] bool empty() const noexcept { return capabilities_.empty(); }

  /// Number of interned capabilities.
  [[nodiscard]] std::size_t size() const noexcept {
    return capabilities_.size();
  }

  /// Removes all interned capabilities (e.g. on STARTTLS renegotiation).
  void clear() noexcept { capabilities_.clear(); }

  [[nodiscard]] const_iterator begin() const noexcept {
    return capabilities_.begin();
  }
  [[nodiscard]] const_iterator end() const noexcept {
    return capabilities_.end();
  }

  /// The allocator this set was constructed with.
  [[nodiscard]] allocator_type get_allocator() const noexcept {
    return allocator_type(capabilities_.get_allocator());
  }

 private:
  container_type capabilities_;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_CAPABILITY_SET_H_
