/**
 * @file include/bkmail/common/mail.h
 * @brief Complete mail message composition.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_COMMON_MAIL_H_
#define BKMAIL_COMMON_MAIL_H_

#include <bkmail/common/envelope.h>
#include <bkmail/common/mail_body.h>
#include <bkmail/common/mail_header.h>

#include <memory>
#include <optional>

namespace bkmail {

/**
 * @brief Complete mail message: header + body, optionally with the IMAP
 * ENVELOPE when produced from FETCH data.
 */
template <class Allocator = std::allocator<std::byte>>
struct mail {
  using allocator_type = Allocator;

  /// Parsed header.
  mail_header<Allocator> header;
  /// Body part.
  mail_body<Allocator> body;
  /// IMAP ENVELOPE, when the message was produced from IMAP FETCH data.
  std::optional<bkmail::envelope<Allocator>> envelope;
};

}  // namespace bkmail

#endif  // BKMAIL_COMMON_MAIL_H_
