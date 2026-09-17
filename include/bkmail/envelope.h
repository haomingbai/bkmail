/**
 * @file include/bkmail/envelope.h
 * @brief IMAP ENVELOPE structure.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_ENVELOPE_H_
#define BKMAIL_ENVELOPE_H_

#include <bkmail/address.h>
#include <bkmail/detail/allocator_ext.h>

#include <memory>

namespace bkmail {

/**
 * @brief The ten RFC 3501 ENVELOPE fields, in wire order.
 *
 * Address fields are address lists; a wire `NIL` list parses to an empty
 * vector, and a wire `NIL` string to an empty string (code_layout D8).
 */
template <class Allocator = std::allocator<std::byte>>
struct envelope {
  using allocator_type = Allocator;
  using string_type = detail::string_of<Allocator>;

  /// Address-list container type used by this envelope.
  using address_list = detail::vector_of<address<Allocator>, Allocator>;

  /// RFC 3501 `env-date` (an RFC 5322 date string, unparsed).
  string_type date;
  /// `env-subject`; empty when NIL.
  string_type subject;
  /// `env-from`.
  address_list from;
  /// `env-sender`.
  address_list sender;
  /// `env-reply-to`.
  address_list reply_to;
  /// `env-to`.
  address_list to;
  /// `env-cc`.
  address_list cc;
  /// `env-bcc`.
  address_list bcc;
  /// `env-in-reply-to`; empty when NIL.
  string_type in_reply_to;
  /// `env-message-id`; empty when NIL.
  string_type message_id;
};

}  // namespace bkmail

#endif  // BKMAIL_ENVELOPE_H_
