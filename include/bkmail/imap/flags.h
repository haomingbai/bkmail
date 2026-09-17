/**
 * @file include/bkmail/imap/flags.h
 * @brief IMAP message system flags and flag set.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_FLAGS_H_
#define BKMAIL_IMAP_FLAGS_H_

#include <cstdint>

namespace bkmail::imap {

/// RFC 3501 system flags (without the leading backslash).
enum class message_flag {
  seen,
  answered,
  flagged,
  deleted,
  draft,
  recent,
};

/**
 * @brief Constexpr bitmask over `message_flag`.
 *
 * Message keywords and the wildcard `\*` are outside this type's scope
 * (code_layout D8).
 */
class flag_set {
 public:
  /// Creates an empty flag set.
  constexpr flag_set() noexcept = default;

  /// Sets or clears flag `f` according to `value`.
  constexpr flag_set& set(message_flag f, bool value = true) noexcept {
    const auto bit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(f));
    bits_ = value ? static_cast<std::uint8_t>(bits_ | bit)
                  : static_cast<std::uint8_t>(bits_ &
                                              static_cast<std::uint8_t>(~bit));
    return *this;
  }

  /// Clears flag `f`.
  constexpr flag_set& reset(message_flag f) noexcept { return set(f, false); }

  /// Returns true when flag `f` is set.
  [[nodiscard]] constexpr bool test(message_flag f) const noexcept {
    return (bits_ &
            static_cast<std::uint8_t>(1u << static_cast<unsigned>(f))) != 0;
  }

  /// Returns true when at least one flag is set.
  [[nodiscard]] constexpr bool any() const noexcept { return bits_ != 0; }

  friend constexpr bool operator==(flag_set, flag_set) = default;

 private:
  std::uint8_t bits_ = 0;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_FLAGS_H_
