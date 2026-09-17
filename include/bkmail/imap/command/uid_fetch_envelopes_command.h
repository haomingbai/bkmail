/**
 * @file include/bkmail/imap/command/uid_fetch_envelopes_command.h
 * @brief UID FETCH ENVELOPE command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 UID FETCH requesting ENVELOPE: `tag UID FETCH <uid-set>
 * (ENVELOPE)`. Identical response handling to fetch_envelopes_command;
 * the UID the server is required to include in every UID FETCH response
 * is consumed tolerantly and not surfaced (the result shape matches the
 * sequence-number variant per docs/code_layout.md §5.2).
 *
 * Handler contract (docs/usage.md §3.3):
 * `void(std::error_code, std::vector<bkmail::envelope<A>, ...>)` in
 * arrival order; a tagged NO is reported as `errc::command_rejected`,
 * a tagged BAD as `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_UID_FETCH_ENVELOPES_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_UID_FETCH_ENVELOPES_COMMAND_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/envelope.h>
#include <bkmail/error.h>
#include <bkmail/imap/detail/fetch_parse.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace bkmail::imap {

/// RFC 3501 UID FETCH command requesting ENVELOPE for a UID set.
template <class Allocator = std::allocator<std::byte>>
class uid_fetch_envelopes_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// One envelope per matched message, in arrival order.
  using result_type =
      bkmail::detail::vector_of<bkmail::envelope<Allocator>, Allocator>;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates the command for a UID set in RFC 3501 syntax.
  explicit uid_fetch_envelopes_command(std::string_view uid_set,
                                       const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        uid_set_(uid_set, char_allocator{alloc}),
        line_(char_allocator{alloc}),
        envelopes_(envelope_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag UID FETCH <uid-set> (ENVELOPE)\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" UID FETCH ");
    line_.append(uid_set_);
    line_.append(" (ENVELOPE)\r\n");
    segment_pending_ = true;
  }

  /// Returns the pending wire segment, or an empty buffer when none.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{line_});
  }

  /// This FETCH form never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// This FETCH form issues no literals.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// Absorbs `* n FETCH (...)` responses and deep-parses the ENVELOPE
  /// attribute; the mandatory UID attribute is skipped tolerantly.
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind != untagged_kind::fetch) return;
    bkmail::envelope<Allocator> env;
    if (detail::parse_envelope_attribute(r.payload, alloc_, env)) {
      envelopes_.push_back(std::move(env));
    }
  }

  /// FIFO ownership (operation_base.h): the `* n FETCH (...)` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::fetch;
  }

  /// Maps the tagged completion: OK fills `out` with the accumulated
  /// envelopes, NO -> command_rejected, BAD -> bad_command.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    const auto ec = detail::map_status_to_ec(r.status);
    if (!ec) out = std::move(envelopes_);
    return ec;
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;
  using envelope_allocator =
      bkmail::detail::rebind_alloc_t<Allocator, bkmail::envelope<Allocator>>;

  [[no_unique_address]] Allocator alloc_;
  string_type uid_set_;
  string_type line_;
  result_type envelopes_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_UID_FETCH_ENVELOPES_COMMAND_H_
