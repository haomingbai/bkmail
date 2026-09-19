/**
 * @file include/bkmail/imap/command/uid_copy_command.h
 * @brief UID COPY command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 UID COPY: `tag UID COPY <uid-set> <mailbox>` — COPY with UID
 * semantics. [TRYCREATE] / [COPYUID] handling matches copy_command: the
 * codes ride inside the tagged response but are not surfaced through the
 * `void` result.
 *
 * Handler contract (docs/usage.md §3.3): `void(std::error_code)`; a
 * tagged NO is reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_UID_COPY_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_UID_COPY_COMMAND_H_

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

/// RFC 3501 UID COPY command (UID semantics).
template <class Allocator = std::allocator<std::byte>>
class uid_copy_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Handler result: UID COPY carries no data.
  using result_type = void;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates a UID COPY command for a UID set and a destination mailbox
  /// name (rendered as an astring).
  uid_copy_command(std::string_view uid_set, std::string_view mailbox,
                   const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        uid_set_(uid_set, char_allocator{alloc}),
        mailbox_(mailbox, char_allocator{alloc}),
        line_(char_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag UID COPY <uid-set> <mailbox>\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" UID COPY ");
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

  /// UID COPY never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// UID COPY issues no literals.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// UID COPY owns no untagged responses.
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

#endif  // BKMAIL_IMAP_COMMAND_UID_COPY_COMMAND_H_
