/**
 * @file include/bkmail/imap/response.h
 * @brief Parsed IMAP server response types.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_RESPONSE_H_
#define BKMAIL_IMAP_RESPONSE_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/imap/flags.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>

namespace bkmail::imap {

/// Status keyword carried by a tagged reply or an untagged status response.
enum class response_status { ok, no, bad, preauth, bye };

/// Classification of an untagged (`*`) response.
enum class untagged_kind {
  ok,          ///< `* OK` status response.
  no,          ///< `* NO` status response.
  bad,         ///< `* BAD` status response.
  preauth,     ///< `* PREAUTH` greeting.
  bye,         ///< `* BYE` (connection-fatal when unsolicited).
  capability,  ///< `* CAPABILITY <atoms>`; payload holds the atom list.
  flags,    ///< `* FLAGS (<flag list>)`; payload holds the parenthesized list.
  list,     ///< `* LIST ...`; payload holds the row.
  lsub,     ///< `* LSUB ...`; payload holds the row.
  status,   ///< `* STATUS <mailbox> (<items>)`; payload holds the remainder.
  search,   ///< `* SEARCH <numbers>`; payload holds the number list.
  exists,   ///< `* <n> EXISTS`; `number` is set.
  recent,   ///< `* <n> RECENT`; `number` is set.
  expunge,  ///< `* <n> EXPUNGE`; `number` is set.
  fetch,    ///< `* <n> FETCH (<attributes>)`; `number` and `payload` are set.
  unknown   ///< Unknown/extension keyword; `payload` holds the raw remainder.
};

// ----- resp-text-code payload forms (RFC 3501 §7.1) -----
// Coded forms without string members are non-templates; the variant is bound
// to an allocator by the `response_code` alias below.

/// `ALERT`: the human-readable text must be presented to the user.
struct alert_code {};

/// `PARSE`: the server could not parse the command.
struct parse_code {};

/// `READ-ONLY`: the mailbox is selected read-only.
struct read_only_code {};

/// `READ-WRITE`: the mailbox is selected read-write.
struct read_write_code {};

/// `TRYCREATE`: the failed command needs a mailbox CREATE first.
struct trycreate_code {};

/// `UIDNEXT <nz-number>`.
struct uidnext_code {
  std::uint32_t value = 0;
};

/// `UIDVALIDITY <nz-number>`.
struct uidvalidity_code {
  std::uint32_t value = 0;
};

/// `UNSEEN <nz-number>`.
struct unseen_code {
  std::uint32_t value = 0;
};

/// `PERMANENTFLAGS (<flag list>)`.
struct permanentflags_code {
  flag_set flags{};
};

/// `BADCHARSET [(<charset list>)]`; the list is empty when none was sent.
template <class Allocator = std::allocator<std::byte>>
struct badcharset_code {
  using string_type = bkmail::detail::string_of<Allocator>;

  bkmail::detail::vector_of<string_type, Allocator> charsets;

  explicit badcharset_code(const Allocator& alloc = Allocator{})
      : charsets(alloc) {}
};

/// `CAPABILITY <atom list>` (the resp-text-code form, not the data response).
template <class Allocator = std::allocator<std::byte>>
struct capability_code {
  using string_type = bkmail::detail::string_of<Allocator>;

  bkmail::detail::vector_of<string_type, Allocator> capabilities;

  explicit capability_code(const Allocator& alloc = Allocator{})
      : capabilities(alloc) {}
};

/**
 * Fallback for any resp-text-code bkmail does not know (or a known code with
 * malformed arguments). Parsing must never fail on an unknown code, so the
 * atom and the raw argument text are preserved here instead.
 */
template <class Allocator = std::allocator<std::byte>>
struct unknown_code {
  using string_type = bkmail::detail::string_of<Allocator>;

  string_type atom;       ///< The code keyword as sent.
  string_type arguments;  ///< Everything after the keyword, up to `]`.

  explicit unknown_code(const Allocator& alloc = Allocator{})
      : atom(alloc), arguments(alloc) {}
};

/// Parsed resp-text-code: a variant over the coded forms of RFC 3501 §7.1
/// plus the `unknown_code` fallback.
template <class Allocator = std::allocator<std::byte>>
using response_code =
    std::variant<alert_code, badcharset_code<Allocator>,
                 capability_code<Allocator>, parse_code, permanentflags_code,
                 read_only_code, read_write_code, trycreate_code, uidnext_code,
                 uidvalidity_code, unseen_code, unknown_code<Allocator>>;

/// A tagged completion reply: `<tag> SP (OK / NO / BAD) SP resp-text`.
template <class Allocator = std::allocator<std::byte>>
struct tagged_response {
  using string_type = bkmail::detail::string_of<Allocator>;

  string_type tag;
  response_status status = response_status::ok;  ///< ok / no / bad only.
  std::optional<response_code<Allocator>> code;  ///< `[resp-code]`, if present.
  string_type text;                              ///< Human-readable text.

  explicit tagged_response(const Allocator& alloc = Allocator{})
      : tag(alloc), text(alloc) {}
};

/**
 * An untagged (`*`) response. For the status kinds (`ok`/`no`/`bad`/
 * `preauth`/`bye`) `status`, `code`, and `text` carry the resp-text. For the
 * numbered kinds (`exists`/`recent`/`expunge`/`fetch`) `number` is set. For
 * the data kinds `payload` is a view of the response remainder (e.g. the
 * parenthesized FETCH attribute list) that aliases the read buffer: it is
 * valid only until the lexer consumes the response, i.e. during dispatch.
 */
template <class Allocator = std::allocator<std::byte>>
struct untagged_response {
  using string_type = bkmail::detail::string_of<Allocator>;

  untagged_kind kind = untagged_kind::unknown;
  std::uint64_t number = 0;
  response_status status = response_status::ok;  ///< Status kinds only.
  std::optional<response_code<Allocator>> code;
  string_type text;
  std::string_view payload;  ///< Dispatch-lifetime view into the read buffer.

  explicit untagged_response(const Allocator& alloc = Allocator{})
      : text(alloc) {}
};

/// A continuation request: `+ SP text` (or `+ SP <base64>` for SASL).
template <class Allocator = std::allocator<std::byte>>
struct continuation_request {
  using string_type = bkmail::detail::string_of<Allocator>;

  string_type text;  ///< Everything after `+`, one leading SP stripped.

  explicit continuation_request(const Allocator& alloc = Allocator{})
      : text(alloc) {}
};

/// One complete server response, classified by its leading token.
template <class Allocator = std::allocator<std::byte>>
using server_response =
    std::variant<tagged_response<Allocator>, untagged_response<Allocator>,
                 continuation_request<Allocator>>;

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_RESPONSE_H_
