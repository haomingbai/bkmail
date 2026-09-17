/**
 * @file include/bkmail/imap/message_attributes.h
 * @brief One FETCH/STORE message datum.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_MESSAGE_ATTRIBUTES_H_
#define BKMAIL_IMAP_MESSAGE_ATTRIBUTES_H_

#include <bkmail/body_structure.h>
#include <bkmail/detail/allocator_ext.h>
#include <bkmail/envelope.h>
#include <bkmail/imap/flags.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace bkmail::imap {

/**
 * @brief One message datum accumulated from a FETCH (or STORE) response.
 *
 * Only the requested items are filled: scalar items keep their default
 * (`0` / empty) when absent — wire UIDs are non-zero, so `uid == 0` reads
 * as "absent" — while `envelope` and `body_structure` are optional.
 * INTERNALDATE is kept as the raw quoted wire string (contract leaves the
 * representation to the model; a string is lossless).
 */
template <class Allocator = std::allocator<std::byte>>
struct message_attributes {
  using allocator_type = Allocator;
  using string_type = bkmail::detail::string_of<Allocator>;

  template <class T>
  using vector_of = bkmail::detail::vector_of<T, Allocator>;

  /// One `BODY[specifier]` section datum.
  struct body_section {
    /// Section specifier as sent, e.g. `"HEADER.FIELDS (FROM TO)"`;
    /// empty for `BODY[]`.
    string_type specifier;
    /// Section octets.
    std::vector<std::byte, Allocator> data;
  };

  /// `FLAGS` item.
  flag_set flags;
  /// `INTERNALDATE` item, unparsed; empty when absent.
  string_type internal_date;
  /// `RFC822.SIZE` item (64-bit); 0 when absent.
  std::uint64_t rfc822_size = 0;
  /// `ENVELOPE` item.
  std::optional<bkmail::envelope<Allocator>> envelope;
  /// `BODYSTRUCTURE` item.
  std::optional<bkmail::body_structure<Allocator>> body_structure;
  /// `BODY[...]` section data, in response order.
  vector_of<body_section> sections;
  /// `UID` item (32-bit); 0 when absent.
  std::uint32_t uid = 0;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_MESSAGE_ATTRIBUTES_H_
