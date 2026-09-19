/**
 * @file include/bkmail/imap/command/close_command.h
 * @brief CLOSE command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 CLOSE: permanently removes all \Deleted messages from the
 * selected mailbox and returns the session to the authenticated state.
 * No untagged responses belong to this command (any `* n EXPUNGE` the
 * server sends is broadcast through the unsolicited path).
 *
 * Handler contract (docs/usage.md §3.3): `void(std::error_code)`; a tagged
 * NO is reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h` — `detail::operation_model` drives it through
 * `render(tag)` / `next_segment()` / `awaits_continuation()` /
 * `on_segment_flushed()` / `on_continuation(text)` / `on_untagged(r)` /
 * `on_tagged(r[, out])`.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_CLOSE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_CLOSE_COMMAND_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/error.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// RFC 3501 CLOSE command. `result_type` is `void`.
template <class Allocator = std::allocator<std::byte>>
class close_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Handler result: CLOSE carries no data.
  using result_type = void;
  /// Rebound character string used for the wire buffer.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates a CLOSE command. No arguments.
  explicit close_command(const Allocator& alloc = Allocator{})
      : alloc_(alloc), line_(char_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag CLOSE\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" CLOSE\r\n");
    segment_pending_ = true;
  }

  /// Returns the pending wire segment, or an empty buffer when none.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{line_});
  }

  /// CLOSE never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// CLOSE issues no literals; a continuation is never addressed to it.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// CLOSE owns no untagged responses.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Maps the tagged completion: OK -> success, NO -> command_rejected,
  /// BAD -> bad_command.
  [[nodiscard]] std::error_code on_tagged(
      const tagged_response<Allocator>& r) const noexcept {
    return detail::map_status_to_ec(r.status);
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;

  [[no_unique_address]] Allocator alloc_;
  string_type line_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_CLOSE_COMMAND_H_
