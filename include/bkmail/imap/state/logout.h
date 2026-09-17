/**
 * @file logout.h
 * @brief IMAP Logout session state.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_STATE_LOGOUT_H_
#define BKMAIL_IMAP_STATE_LOGOUT_H_

#include <memory>

namespace bkmail::imap {

/**
 * Terminal session state.
 *
 * Carries no data: its existence only proves the LOGOUT exchange finished.
 * The connection is torn down by the logout operation before this state is
 * delivered, so there is nothing left to operate on.
 */
template <class Allocator = std::allocator<std::byte>>
class logout_state {
 public:
  using allocator_type = Allocator;

  /// Constructs the terminal proof object.
  logout_state() = default;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_STATE_LOGOUT_H_
