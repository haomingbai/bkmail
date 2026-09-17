/**
 * @file include/bkmail/mail_body.h
 * @brief Owning message body with content metadata.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_MAIL_BODY_H_
#define BKMAIL_MAIL_BODY_H_

#include <bkmail/detail/allocator_ext.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace bkmail {

/**
 * @brief Owning message body plus content metadata.
 *
 * `data` holds the body octets as fetched (decoded-or-raw per the
 * producing operation); `content_type` carries the full Content-Type
 * field value, e.g. `"text/plain; charset=utf-8"`.
 */
template <class Allocator = std::allocator<std::byte>>
struct mail_body {
  using allocator_type = Allocator;
  using string_type = detail::string_of<Allocator>;

  /// Owning body octet container; uses `Allocator` directly.
  using data_type = std::vector<std::byte, Allocator>;

  /// Content-Type field value.
  string_type content_type;
  /// Body octets.
  data_type data;
};

}  // namespace bkmail

#endif  // BKMAIL_MAIL_BODY_H_
