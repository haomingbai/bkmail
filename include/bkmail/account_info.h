/**
 * @file include/bkmail/account_info.h
 * @brief IMAP account credentials carrier.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_ACCOUNT_INFO_H_
#define BKMAIL_ACCOUNT_INFO_H_

#include <bkmail/detail/allocator_ext.h>

#include <memory>
#include <optional>

namespace bkmail {

/**
 * @brief User identity for IMAP authentication.
 *
 * Credential carrier only; it holds no server settings. An aggregate, so
 * designated initializers work:
 * `account_info<>{.user_name = "u", .password = "p"}`.
 */
template <class Allocator = std::allocator<std::byte>>
struct account_info {
  using allocator_type = Allocator;
  using string_type = detail::string_of<Allocator>;

  /// IMAP user name.
  string_type user_name;
  /// IMAP password.
  string_type password;
  /// SASL authorization identity (authzid), when acting on behalf of
  /// another user.
  std::optional<string_type> authzid;
};

}  // namespace bkmail

#endif  // BKMAIL_ACCOUNT_INFO_H_
