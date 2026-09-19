/**
 * @file include/bkmail/imap/command/fetch_command.h
 * @brief Generic FETCH command over fetch_items.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 FETCH over an arbitrary imap::fetch_items selection:
 * `tag FETCH <seq-set> (<item> ...)`. The bitmask items render as their
 * atoms (ENVELOPE, BODYSTRUCTURE, FLAGS, UID, INTERNALDATE, RFC822.SIZE);
 * each fetch_items::section renders as `BODY[specifier]` or
 * `BODY.PEEK[specifier]`.
 *
 * Each `* n FETCH (...)` response is parsed attribute by attribute
 * (imap/detail/fetch_parse.h); attributes may arrive in any order and
 * unknown ones are skipped tolerantly. BODY[...] payloads land on
 * message_attributes::sections; RFC822 / RFC822.HEADER / RFC822.TEXT
 * payloads are consumed and dropped (no message_attributes member models
 * them).
 *
 * Handler contract (docs/usage.md §3.3):
 * `void(std::error_code, std::vector<imap::message_attributes<A>, ...>)`
 * in arrival order; a tagged NO is reported as `errc::command_rejected`,
 * a tagged BAD as `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_FETCH_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_FETCH_COMMAND_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/error.h>
#include <bkmail/imap/detail/fetch_parse.h>
#include <bkmail/imap/fetch_items.h>
#include <bkmail/imap/message_attributes.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace bkmail::imap {

/// RFC 3501 generic FETCH command driven by an imap::fetch_items
/// selection.
template <class Allocator = std::allocator<std::byte>>
class fetch_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// One datum per matched message, in arrival order.
  using result_type =
      bkmail::detail::vector_of<message_attributes<Allocator>, Allocator>;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates the command for a sequence set and an item selection.
  fetch_command(std::string_view seq_set, fetch_items items,
                const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        seq_set_(seq_set, char_allocator{alloc}),
        items_(std::move(items)),
        line_(char_allocator{alloc}),
        data_(attributes_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag FETCH <seq-set> (<items>)\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" FETCH ");
    line_.append(seq_set_);
    line_.append(" (");
    append_items(line_);
    // Trim the trailing space left by append_items, if any item was set.
    if (!line_.empty() && line_.back() == ' ') line_.pop_back();
    line_.append(")\r\n");
    segment_pending_ = true;
  }

  /// Returns the pending wire segment, or an empty buffer when none.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (!segment_pending_) return {};
    return bnio::buffer(std::string_view{line_});
  }

  /// FETCH never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the current segment consumed by the write pump.
  void on_segment_flushed() noexcept { segment_pending_ = false; }

  /// FETCH issues no literals.
  void on_continuation(std::string_view /*text*/) noexcept {}

  /// Absorbs `* n FETCH (...)` responses and parses every recognized
  /// attribute into one message_attributes per response line.
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind != untagged_kind::fetch) return;
    message_attributes<Allocator> datum;
    if (detail::parse_fetch_attributes(r.payload, alloc_, datum)) {
      data_.push_back(std::move(datum));
    }
  }

  /// FIFO ownership (operation_base.h): the `* n FETCH (...)` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::fetch;
  }

  /// Maps the tagged completion: OK fills `out` with the accumulated
  /// data, NO -> command_rejected, BAD -> bad_command.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    const auto ec = detail::map_status_to_ec(r.status);
    if (!ec) out = std::move(data_);
    return ec;
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;
  using attributes_allocator =
      bkmail::detail::rebind_alloc_t<Allocator, message_attributes<Allocator>>;

  /// Appends the wire names of the requested items, each space-terminated.
  void append_items(string_type& out) const {
    using item = fetch_items::item;
    if (items_.test(item::envelope)) out.append("ENVELOPE ");
    if (items_.test(item::body_structure)) out.append("BODYSTRUCTURE ");
    if (items_.test(item::flags)) out.append("FLAGS ");
    if (items_.test(item::uid)) out.append("UID ");
    if (items_.test(item::internal_date)) out.append("INTERNALDATE ");
    if (items_.test(item::rfc822_size)) out.append("RFC822.SIZE ");
    for (const fetch_items::section& sec : items_.sections()) {
      out.append(sec.peek ? "BODY.PEEK[" : "BODY[");
      out.append(sec.specifier);
      out.append("] ");
    }
  }

  [[no_unique_address]] Allocator alloc_;
  string_type seq_set_;
  fetch_items items_;
  string_type line_;
  result_type data_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_FETCH_COMMAND_H_
