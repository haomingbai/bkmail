/**
 * @file include/bkmail/imap/command/fetch_envelopes_command.h
 * @brief FETCH ENVELOPE command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 FETCH over a message sequence set requesting ENVELOPE only:
 * `tag FETCH <seq-set> (ENVELOPE)`. The server answers with one
 * `* n FETCH (ENVELOPE (...))` per message; attribute order inside the
 * response is irrelevant because the ENVELOPE attribute is located by
 * name and deep-parsed (imap/detail/fetch_parse.h). Extra attributes the
 * server volunteers (e.g. FLAGS) are skipped tolerantly.
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
#ifndef BKMAIL_IMAP_COMMAND_FETCH_ENVELOPES_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_FETCH_ENVELOPES_COMMAND_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/envelope.h>
#include <bkmail/common/error.h>
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

/// RFC 3501 FETCH command requesting ENVELOPE for a sequence set.
template <class Allocator = std::allocator<std::byte>>
class fetch_envelopes_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// One envelope per matched message, in arrival order.
  using result_type =
      bkmail::detail::vector_of<bkmail::envelope<Allocator>, Allocator>;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates the command for a sequence set in RFC 3501 syntax
  /// (`"1:5"`, `"2:4,7,9:*"`). The set is not interpreted here; the
  /// validated value type lives in imap/sequence_set.h.
  explicit fetch_envelopes_command(std::string_view seq_set,
                                   const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        seq_set_(seq_set, char_allocator{alloc}),
        line_(char_allocator{alloc}),
        envelopes_(envelope_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag FETCH <seq-set> (ENVELOPE)\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" FETCH ");
    line_.append(seq_set_);
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
  /// attribute; unrelated untagged responses are ignored.
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
  string_type seq_set_;
  string_type line_;
  result_type envelopes_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_FETCH_ENVELOPES_COMMAND_H_
