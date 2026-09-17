/**
 * @file bkmail.h
 * @brief Aggregate header for the entire bkmail library.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_BKMAIL_H_
#define BKMAIL_BKMAIL_H_

#include <bkmail/export.h>
#include <bkmail/version.h>

// Public dependencies whose types flow through bkmail's interface.
#include <bnio/bnio.h>

#include <bexec/bexec.hpp>

// Core: errors, credentials, mail data model, sender helpers.
#include <bkmail/account_info.h>
#include <bkmail/address.h>
#include <bkmail/body_structure.h>
#include <bkmail/envelope.h>
#include <bkmail/error.h>
#include <bkmail/mail.h>
#include <bkmail/mail_body.h>
#include <bkmail/mail_header.h>
#include <bkmail/pack.h>

// IMAP facility (layers 1+2 and the imap-scoped model types) and the
// session connect entry points.
#include <bkmail/connect.h>
#include <bkmail/imap.h>

#endif  // BKMAIL_BKMAIL_H_
