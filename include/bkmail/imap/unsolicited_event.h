/**
 * @file include/bkmail/imap/unsolicited_event.h
 * @brief Unsolicited server event types.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_UNSOLICITED_EVENT_H_
#define BKMAIL_IMAP_UNSOLICITED_EVENT_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/imap/capability_set.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/response.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace bkmail::imap {

/// `* <n> EXISTS` push: the mailbox now holds `count` messages.
struct exists_event {
  std::uint32_t count = 0;
};

/// `* <n> RECENT` push.
struct recent_event {
  std::uint32_t count = 0;
};

/// `* <n> EXPUNGE` push: the message with `sequence_number` is gone and all
/// later sequence numbers shift down by one.
struct expunge_event {
  std::uint32_t sequence_number = 0;
};

/// Unsolicited flag change on one message (`* <n> FETCH (FLAGS (...))`).
struct flags_update_event {
  std::uint32_t sequence_number = 0;
  flag_set flags{};
};

/// `* CAPABILITY` push: the server announced a new capability set.
template <class Allocator = std::allocator<std::byte>>
struct capability_event {
  capability_set<Allocator> capabilities;

  explicit capability_event(const Allocator& alloc = Allocator{})
      : capabilities(alloc) {}
};

/// `* BYE` push: the server is closing the connection; `code` (e.g. `ALERT`)
/// and `text` describe why.
template <class Allocator = std::allocator<std::byte>>
struct bye_event {
  using string_type = bkmail::detail::string_of<Allocator>;

  string_type text;
  std::optional<response_code<Allocator>> code;

  explicit bye_event(const Allocator& alloc = Allocator{}) : text(alloc) {}
};

/**
 * Greeting report: an untagged status response with the `OK` or `PREAUTH`
 * status keyword. The first line of a session is the greeting (OK selects
 * the Not-Authenticated state, PREAUTH the Authenticated one); later
 * untagged OKs (resp-code carriers) surface through the same event and are
 * ignorable by login flows (docs/usage.md §3.5).
 */
template <class Allocator = std::allocator<std::byte>>
struct greeting_event {
  using string_type = bkmail::detail::string_of<Allocator>;

  /// `response_status::ok` or `response_status::preauth`.
  response_status status = response_status::ok;
  string_type text;
  std::optional<response_code<Allocator>> code;

  explicit greeting_event(const Allocator& alloc = Allocator{}) : text(alloc) {}
};

/// The unsolicited-event variant handed to `on_unsolicited` handlers.
template <class Allocator = std::allocator<std::byte>>
using unsolicited_event =
    std::variant<exists_event, recent_event, expunge_event, flags_update_event,
                 capability_event<Allocator>, bye_event<Allocator>,
                 greeting_event<Allocator>>;

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_UNSOLICITED_EVENT_H_
