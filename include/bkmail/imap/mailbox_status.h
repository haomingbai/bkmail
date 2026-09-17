/**
 * @file include/bkmail/imap/mailbox_status.h
 * @brief STATUS command result.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_MAILBOX_STATUS_H_
#define BKMAIL_IMAP_MAILBOX_STATUS_H_

#include <cstdint>
#include <memory>
#include <optional>

namespace bkmail::imap {

/**
 * @brief Result of a STATUS command.
 *
 * A STATUS response carries only the requested items, so every field is
 * optional: a disengaged field means the item was not requested (or not
 * returned), not that it is zero.
 */
template <class Allocator = std::allocator<std::byte>>
struct mailbox_status {
  using allocator_type = Allocator;

  /// `MESSAGES`: number of messages in the mailbox.
  std::optional<std::uint32_t> messages;
  /// `RECENT`: number of messages with the `\Recent` flag.
  std::optional<std::uint32_t> recent;
  /// `UIDNEXT`: next predicted UID.
  std::optional<std::uint32_t> uid_next;
  /// `UIDVALIDITY`: UID validity value (32-bit).
  std::optional<std::uint32_t> uid_validity;
  /// `UNSEEN`: number of messages without the `\Seen` flag.
  std::optional<std::uint32_t> unseen;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_MAILBOX_STATUS_H_
