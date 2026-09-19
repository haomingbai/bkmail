/**
 * @file include/bkmail/imap/command/expunge_command.h
 * @brief EXPUNGE command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 EXPUNGE: permanently removes all \Deleted messages from the
 * selected mailbox. The server answers with zero or more `* n EXPUNGE`
 * untagged responses (also broadcast as expunge_event through the
 * unsolicited path); they are tolerated here and carry no result.
 *
 * Handler contract (docs/usage.md §3.3): `void(std::error_code)`; a tagged
 * NO is reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_EXPUNGE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_EXPUNGE_COMMAND_H_

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

/// RFC 3501 EXPUNGE command. `result_type` is `void`.
template <class Allocator = std::allocator<std::byte>>
class expunge_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Handler result: EXPUNGE carries no data.
  using result_type = void;
  /// Rebound character string used for the wire buffer.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates an EXPUNGE command. No arguments.
  explicit expunge_command(const Allocator& alloc = Allocator{})
      : alloc_(alloc), line_(char_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag EXPUNGE\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" EXPUNGE\r\n");
    segment_pending_ = true;
  }

  /// Returns the pending wire segment, or an empty buffer when none.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{line_});
  }

  /// EXPUNGE never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// EXPUNGE issues no literals; a continuation is never addressed to it.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// `* n EXPUNGE` responses belong to this command but carry no result;
  /// they are also delivered as expunge_event on the unsolicited path.
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

#endif  // BKMAIL_IMAP_COMMAND_EXPUNGE_COMMAND_H_
