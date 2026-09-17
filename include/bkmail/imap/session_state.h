/**
 * @file session_state.h
 * @brief Forward declarations of the five Layer-2 session states.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_SESSION_STATE_H_
#define BKMAIL_IMAP_SESSION_STATE_H_

namespace bkmail::imap {

// The five Layer-2 session states (see imap/state/*.h). They form one
// coupled group: every operation consumes its state by rvalue and delivers
// the successor state for the observed server outcome through its sender —
// one set_value signature per successor, never a merged variant (variant
// merging is a consumer-side choice, e.g. bexec::into_variant). The default
// allocator argument lives on the definitions, not on these declarations.
template <class Allocator>
class not_authenticated_state;
template <class Allocator>
class authenticated_state;
template <class Allocator>
class selected_state;
template <class Allocator>
class logout_state;
template <class Allocator>
class idle_state;

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_SESSION_STATE_H_
