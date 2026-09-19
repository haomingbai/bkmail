/**
 * @file include/bkmail/imap/command/uid_move_command.h
 * @brief UID MOVE command (UIDPLUS).
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 6851 (UIDPLUS) UID MOVE: `tag UID MOVE <uid-set> <mailbox>`.
 * Capability gating is the state layer's job, exactly as for
 * move_command; [TRYCREATE] / [COPYUID] ride inside the tagged response
 * but are not surfaced through the `void` result.
 *
 * Handler contract (docs/usage.md §3.3): `void(std::error_code)`; a
 * tagged NO is reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_UID_MOVE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_UID_MOVE_COMMAND_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/error.h>
#include <bkmail/imap/detail/astring.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// RFC 6851 UID MOVE command (UIDPLUS, UID semantics).
template <class Allocator = std::allocator<std::byte>>
class uid_move_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Handler result: UID MOVE carries no data.
  using result_type = void;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates a UID MOVE command for a UID set and a destination mailbox
  /// name (rendered as an astring).
  uid_move_command(std::string_view uid_set, std::string_view mailbox,
                   const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        uid_set_(uid_set, char_allocator{alloc}),
        mailbox_(mailbox, char_allocator{alloc}),
        line_(char_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag UID MOVE <uid-set> <mailbox>\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" UID MOVE ");
    line_.append(uid_set_);
    line_.push_back(' ');
    detail::render_astring(std::string_view{mailbox_}, line_);
    line_.append("\r\n");
    segment_pending_ = true;
  }

  /// Returns the pending wire segment, or an empty buffer when none.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{line_});
  }

  /// UID MOVE never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// UID MOVE issues no literals.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// `* n EXPUNGE` responses a UID MOVE provokes are broadcast on the
  /// unsolicited path; nothing is absorbed here.
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
  string_type uid_set_;
  string_type mailbox_;
  string_type line_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_UID_MOVE_COMMAND_H_
