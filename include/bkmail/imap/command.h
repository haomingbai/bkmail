/**
 * @file include/bkmail/imap/command.h
 * @brief Aggregate header for all IMAP command types.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Pulls in every imap/command/*.h command type plus the type-erased
 * imap_command handle and make_command. Aggregate header: includes only,
 * no logic.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_H_

#include <bkmail/imap/command/append_command.h>
#include <bkmail/imap/command/authenticate_command.h>
#include <bkmail/imap/command/capability_command.h>
#include <bkmail/imap/command/close_command.h>
#include <bkmail/imap/command/copy_command.h>
#include <bkmail/imap/command/create_command.h>
#include <bkmail/imap/command/delete_command.h>
#include <bkmail/imap/command/examine_command.h>
#include <bkmail/imap/command/expunge_command.h>
#include <bkmail/imap/command/fetch_command.h>
#include <bkmail/imap/command/fetch_envelopes_command.h>
#include <bkmail/imap/command/fetch_headers_command.h>
#include <bkmail/imap/command/fetch_message_command.h>
#include <bkmail/imap/command/idle_command.h>
#include <bkmail/imap/command/list_command.h>
#include <bkmail/imap/command/login_command.h>
#include <bkmail/imap/command/logout_command.h>
#include <bkmail/imap/command/move_command.h>
#include <bkmail/imap/command/noop_command.h>
#include <bkmail/imap/command/raw_command.h>
#include <bkmail/imap/command/rename_command.h>
#include <bkmail/imap/command/search_command.h>
#include <bkmail/imap/command/select_command.h>
#include <bkmail/imap/command/starttls_command.h>
#include <bkmail/imap/command/status_command.h>
#include <bkmail/imap/command/store_command.h>
#include <bkmail/imap/command/subscribe_command.h>
#include <bkmail/imap/command/uid_copy_command.h>
#include <bkmail/imap/command/uid_fetch_command.h>
#include <bkmail/imap/command/uid_fetch_envelopes_command.h>
#include <bkmail/imap/command/uid_fetch_headers_command.h>
#include <bkmail/imap/command/uid_fetch_message_command.h>
#include <bkmail/imap/command/uid_move_command.h>
#include <bkmail/imap/command/uid_search_command.h>
#include <bkmail/imap/command/uid_store_command.h>
#include <bkmail/imap/command/unsubscribe_command.h>
#include <bkmail/imap/imap_command.h>

#endif  // BKMAIL_IMAP_COMMAND_H_
