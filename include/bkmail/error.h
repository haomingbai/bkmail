/**
 * @file include/bkmail/error.h
 * @brief bkmail error codes and error category.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_ERROR_H_
#define BKMAIL_ERROR_H_

#include <bkmail/export.h>

#include <system_error>
#include <type_traits>

namespace bkmail {

/**
 * @brief bkmail protocol-level error conditions.
 *
 * Values produced in `bkmail::error_category()`. Following the bnio/bexec
 * completion contract, these travel on the value channel of senders as
 * `std::error_code`, never through exceptions or `set_error`.
 */
enum class errc {
  /// Tagged `NO`: the server rejected the command (bad credentials, no such
  /// mailbox, ...). Recoverable: the session stays usable.
  command_rejected = 1,
  /// Tagged `BAD`: command unknown or malformed (usually a client bug).
  /// Recoverable: the session stays usable.
  bad_command,
  /// Untagged `BYE` outside a logout exchange; fatal to the session.
  server_bye,
  /// Unparseable or out-of-order server output; fatal to the session.
  unexpected_response,
  /// The operation needs an extension the server does not advertise.
  capability_required,
};

/// Returns the bkmail error category singleton.
[[nodiscard]] BKMAIL_EXPORT const std::error_category&
error_category() noexcept;

/// Builds an `std::error_code` from a `bkmail::errc` value.
[[nodiscard]] BKMAIL_EXPORT std::error_code make_error_code(errc e) noexcept;

}  // namespace bkmail

namespace std {

/// Marks `bkmail::errc` as an error-code enum for implicit conversion.
template <>
struct is_error_code_enum<bkmail::errc> : true_type {};

}  // namespace std

#endif  // BKMAIL_ERROR_H_
