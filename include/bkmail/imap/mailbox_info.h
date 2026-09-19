/**
 * @file include/bkmail/imap/mailbox_info.h
 * @brief SELECT/EXAMINE mailbox snapshot.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_MAILBOX_INFO_H_
#define BKMAIL_IMAP_MAILBOX_INFO_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/imap/flags.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace bkmail::imap {

/// STATUS data items, as a bitmask (RFC 3501 `status-att-list`).
enum class status_items : std::uint8_t {
  none = 0,
  messages = 1u << 0,
  recent = 1u << 1,
  uid_next = 1u << 2,
  uid_validity = 1u << 3,
  unseen = 1u << 4,
};

/// Bitwise-or of two status-items masks.
[[nodiscard]] constexpr status_items operator|(status_items a,
                                               status_items b) noexcept {
  return static_cast<status_items>(static_cast<std::uint8_t>(a) |
                                   static_cast<std::uint8_t>(b));
}

/// Bitwise-and of two status-items masks.
[[nodiscard]] constexpr status_items operator&(status_items a,
                                               status_items b) noexcept {
  return static_cast<status_items>(static_cast<std::uint8_t>(a) &
                                   static_cast<std::uint8_t>(b));
}

/// In-place bitwise-or of a status-items mask.
constexpr status_items& operator|=(status_items& a, status_items b) noexcept {
  return a = a | b;
}

/// Returns true when every flag of `flag` is present in `set`.
[[nodiscard]] constexpr bool has_status_item(status_items set,
                                             status_items flag) noexcept {
  return (set & flag) == flag;
}

/**
 * @brief Snapshot of a selected/examined mailbox.
 *
 * Filled from the untagged responses and response codes of SELECT/EXAMINE
 * and maintained from `EXISTS`/`EXPUNGE` pushes while selected;
 * `selected_state::mailbox()` returns a `const&` to one of these.
 */
template <class Allocator = std::allocator<std::byte>>
struct mailbox_info {
  using allocator_type = Allocator;
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Mailbox name.
  string_type name;
  /// Message count (`* n EXISTS`).
  std::uint32_t exists = 0;
  /// `\Recent` count (`* n RECENT`).
  std::uint32_t recent = 0;
  /// First unseen sequence number (`[UNSEEN n]`); disengaged when the
  /// server sent no UNSEEN code (NIL is semantically distinct here,
  /// code_layout D8).
  std::optional<std::uint32_t> unseen;
  /// UIDVALIDITY (32-bit).
  std::uint32_t uid_validity = 0;
  /// Next predicted UID (`[UIDNEXT n]`).
  std::uint32_t uid_next = 0;
  /// Flags defined in the mailbox (`* FLAGS (...)`).
  flag_set flags;
  /// Flags storable permanently (`[PERMANENTFLAGS (...)]`).
  flag_set permanent_flags;
  /// True after EXAMINE or a `[READ-ONLY]` SELECT.
  bool read_only = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_MAILBOX_INFO_H_
