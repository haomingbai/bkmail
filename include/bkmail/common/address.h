/**
 * @file include/bkmail/common/address.h
 * @brief RFC 3501 address tuple.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_COMMON_ADDRESS_H_
#define BKMAIL_COMMON_ADDRESS_H_

#include <bkmail/common/detail/allocator_ext.h>

#include <memory>

namespace bkmail {

/**
 * @brief One RFC 3501 `address` tuple `(name, adl, mailbox, host)`.
 *
 * Wire `NIL` is represented by an empty string (code_layout D8). `adl` is
 * the at-domain list (source route), obsolete in practice and usually NIL;
 * it is kept for wire fidelity.
 */
template <class Allocator = std::allocator<std::byte>>
struct address {
  using allocator_type = Allocator;
  using string_type = detail::string_of<Allocator>;

  /// addr-name: display name phrase; empty when NIL.
  string_type display_name;
  /// addr-adl: at-domain list (source route); empty when NIL.
  string_type adl;
  /// addr-mailbox: local part.
  string_type mailbox_name;
  /// addr-host: domain; empty when NIL.
  string_type host_name;

  /// Returns `"mailbox_name@host_name"`, or just `mailbox_name` when
  /// `host_name` is empty. The result reuses this address's allocator.
  [[nodiscard]] string_type email() const {
    string_type result(mailbox_name.get_allocator());
    result.reserve(mailbox_name.size() + host_name.size() + 1);
    result += mailbox_name;
    if (!host_name.empty()) {
      result += '@';
      result += host_name;
    }
    return result;
  }
};

}  // namespace bkmail

#endif  // BKMAIL_COMMON_ADDRESS_H_
