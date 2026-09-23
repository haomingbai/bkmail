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
#include <bkmail/common/account_info.h>
#include <bkmail/common/address.h>
#include <bkmail/common/body_structure.h>
#include <bkmail/common/envelope.h>
#include <bkmail/common/error.h>
#include <bkmail/common/mail.h>
#include <bkmail/common/mail_body.h>
#include <bkmail/common/mail_header.h>
#include <bkmail/common/pack.h>

// IMAP facility (layers 1+2 and the imap-scoped model types) and the
// session connect entry points.
#include <bkmail/imap.h>
#include <bkmail/imap/connect.h>

#endif  // BKMAIL_BKMAIL_H_
