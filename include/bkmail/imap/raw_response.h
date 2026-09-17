/**
 * @file include/bkmail/imap/raw_response.h
 * @brief Tagged reply payload of a raw command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_RAW_RESPONSE_H_
#define BKMAIL_IMAP_RAW_RESPONSE_H_

#include <bkmail/detail/allocator_ext.h>

#include <memory>

namespace bkmail::imap {

/**
 * @brief Tagged reply payload of a `raw_command`.
 *
 * Carries only the tagged reply's essentials; untagged responses the
 * command elicited are not collected.
 */
template <class Allocator = std::allocator<std::byte>>
struct raw_response {
  using allocator_type = Allocator;
  using string_type = bkmail::detail::string_of<Allocator>;

  /// The command's tag.
  string_type tag;
  /// True for tagged `OK`; false for `NO`/`BAD`.
  bool ok = false;
  /// Response text after the status atom.
  string_type text;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_RAW_RESPONSE_H_
