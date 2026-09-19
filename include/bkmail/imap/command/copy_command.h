/**
 * @file include/bkmail/imap/command/copy_command.h
 * @brief COPY command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 COPY: `tag COPY <seq-set> <mailbox>`. On failure the server
 * may hint `[TRYCREATE]` in the tagged NO; with UIDPLUS a success carries
 * `[COPYUID <uidvalidity> <src-uids> <dst-uids>]`. Both ride inside the
 * tagged response's resp-text-code; the callback-path result_type is
 * `void` per the naming master table, so the code is not surfaced to the
 * handler — callers needing the COPYUID mapping use raw_command or parse
 * the tagged_response on a custom engine hook.
 *
 * Handler contract (docs/usage.md §3.3): `void(std::error_code)`; a
 * tagged NO is reported as `errc::command_rejected`, a tagged BAD as
 * `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_COPY_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_COPY_COMMAND_H_

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

/// RFC 3501 COPY command over message sequence numbers.
template <class Allocator = std::allocator<std::byte>>
class copy_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// Handler result: COPY carries no data.
  using result_type = void;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates a COPY command for a sequence set and a destination mailbox
  /// name (rendered as an astring).
  copy_command(std::string_view seq_set, std::string_view mailbox,
               const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        seq_set_(seq_set, char_allocator{alloc}),
        mailbox_(mailbox, char_allocator{alloc}),
        line_(char_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag COPY <seq-set> <mailbox>\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" COPY ");
    line_.append(seq_set_);
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

  /// COPY never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// COPY issues no literals.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// COPY owns no untagged responses.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Maps the tagged completion: OK -> success, NO -> command_rejected
  /// (a [TRYCREATE] hint rides along inside it), BAD -> bad_command.
  [[nodiscard]] std::error_code on_tagged(
      const tagged_response<Allocator>& r) const noexcept {
    return detail::map_status_to_ec(r.status);
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;

  [[no_unique_address]] Allocator alloc_;
  string_type seq_set_;
  string_type mailbox_;
  string_type line_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_COPY_COMMAND_H_
