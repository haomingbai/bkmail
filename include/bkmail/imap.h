/**
 * @file imap.h
 * @brief Aggregate header for the bkmail IMAP facility.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_H_
#define BKMAIL_IMAP_H_

// imap-scoped data model and wire types.
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/fetch_items.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/mailbox_entry.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/mailbox_status.h>
#include <bkmail/imap/message_attributes.h>
#include <bkmail/imap/raw_response.h>
#include <bkmail/imap/response.h>
#include <bkmail/imap/search_criteria.h>
#include <bkmail/imap/sequence_set.h>
#include <bkmail/imap/unsolicited_event.h>

// Layer 1: command/connection layer.
#include <bkmail/imap/command.h>
#include <bkmail/imap/imap_command.h>
#include <bkmail/imap/imap_context.h>

// Layer 2: session state machine.
#include <bkmail/imap/imap_connection.h>
#include <bkmail/imap/session_state.h>
#include <bkmail/imap/state/authenticated.h>
#include <bkmail/imap/state/logout.h>
#include <bkmail/imap/state/not_authenticated.h>
#include <bkmail/imap/state/selected.h>

#endif  // BKMAIL_IMAP_H_
