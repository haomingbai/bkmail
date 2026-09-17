/**
 * @file include/bkmail/imap/command/fetch_headers_command.h
 * @brief FETCH BODY.PEEK[HEADER.FIELDS] command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 FETCH requesting selected header fields without setting \Seen:
 * `tag FETCH <seq-set> (BODY.PEEK[HEADER.FIELDS (<field> ...)])`. The
 * server answers with `* n FETCH (BODY[HEADER.FIELDS (...)] <nstring>)`
 * where the value is a quoted string or a literal; the header block is
 * unfolded and mapped onto bkmail::mail_header
 * (imap/detail/fetch_parse.h). Values are stored as received; RFC 2047
 * decoding is the decode_encoded_words caller's job (mail_header.h).
 *
 * Handler contract (docs/usage.md §3.3):
 * `void(std::error_code, std::vector<bkmail::mail_header<A>, ...>)` in
 * arrival order; a tagged NO is reported as `errc::command_rejected`,
 * a tagged BAD as `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_FETCH_HEADERS_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_FETCH_HEADERS_COMMAND_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/error.h>
#include <bkmail/imap/detail/astring.h>
#include <bkmail/imap/detail/fetch_parse.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bkmail/mail_header.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bkmail::imap {

/// RFC 3501 FETCH command requesting selected header fields via
/// BODY.PEEK[HEADER.FIELDS (...)] for a sequence set.
template <class Allocator = std::allocator<std::byte>>
class fetch_headers_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// One parsed header view per matched message, in arrival order.
  using result_type =
      bkmail::detail::vector_of<bkmail::mail_header<Allocator>, Allocator>;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates the command for a sequence set and the header field names to
  /// retrieve (`{"Subject", "From"}`). Field names are rendered as
  /// astrings via detail::render_astring.
  fetch_headers_command(std::string_view seq_set,
                        std::vector<std::string_view> fields,
                        const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        seq_set_(seq_set, char_allocator{alloc}),
        fields_(field_allocator{alloc}),
        line_(char_allocator{alloc}),
        headers_(header_allocator{alloc}) {
    for (const std::string_view field : fields) {
      fields_.emplace_back(field, char_allocator{alloc});
    }
  }

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders
  /// `tag FETCH <seq-set> (BODY.PEEK[HEADER.FIELDS (<fields>)])\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" FETCH ");
    line_.append(seq_set_);
    line_.append(" (BODY.PEEK[HEADER.FIELDS (");
    bool first = true;
    for (const string_type& field : fields_) {
      if (!first) line_.push_back(' ');
      first = false;
      detail::render_astring(std::string_view{field}, line_);
    }
    line_.append(")])\r\n");
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

  /// Absorbs `* n FETCH (BODY[HEADER.FIELDS (...)] <nstring>)` responses
  /// and parses the header block; unrelated responses are ignored.
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind != untagged_kind::fetch) return;
    std::optional<string_type> block;
    if (!detail::parse_body_section_attribute(r.payload, "BODY[HEADER.FIELDS",
                                              alloc_, block) ||
        !block) {
      return;
    }
    bkmail::mail_header<Allocator> header;
    detail::parse_mail_header_block(std::string_view{*block}, alloc_, header);
    headers_.push_back(std::move(header));
  }

  /// FIFO ownership (operation_base.h): the `* n FETCH (...)` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::fetch;
  }

  /// Maps the tagged completion: OK fills `out` with the accumulated
  /// headers, NO -> command_rejected, BAD -> bad_command.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    const auto ec = detail::map_status_to_ec(r.status);
    if (!ec) out = std::move(headers_);
    return ec;
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;
  using field_allocator =
      bkmail::detail::rebind_alloc_t<Allocator, string_type>;
  using header_allocator =
      bkmail::detail::rebind_alloc_t<Allocator, bkmail::mail_header<Allocator>>;

  [[no_unique_address]] Allocator alloc_;
  string_type seq_set_;
  bkmail::detail::vector_of<string_type, Allocator> fields_;
  string_type line_;
  result_type headers_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_FETCH_HEADERS_COMMAND_H_
