/**
 * @file include/bkmail/imap/mailbox_entry.h
 * @brief One LIST response entry.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_MAILBOX_ENTRY_H_
#define BKMAIL_IMAP_MAILBOX_ENTRY_H_

#include <bkmail/common/detail/allocator_ext.h>

#include <memory>

namespace bkmail::imap {

/**
 * @brief One row of a LIST response.
 *
 * Only the common attributes are broken out (`no_select`, `has_children`,
 * `has_no_children`); a NIL hierarchy delimiter is an empty `delimiter`.
 */
template <class Allocator = std::allocator<std::byte>>
struct mailbox_entry {
  using allocator_type = Allocator;
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Mailbox name.
  string_type name;
  /// Hierarchy delimiter; empty when the wire sent NIL.
  string_type delimiter;
  /// `\NoSelect`: the mailbox cannot be selected.
  bool no_select = false;
  /// `\HasChildren`.
  bool has_children = false;
  /// `\HasNoChildren`.
  bool has_no_children = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_MAILBOX_ENTRY_H_
