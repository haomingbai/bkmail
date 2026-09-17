/**
 * @file include/bkmail/imap/command/fetch_message_command.h
 * @brief FETCH BODY.PEEK[] full-message command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * RFC 3501 FETCH requesting the entire message without setting \Seen:
 * `tag FETCH <seq-set> (BODY.PEEK[])`. The server answers with
 * `* n FETCH (BODY[] <nstring>)`; the value (typically a literal) is
 * split at the first empty line into an RFC 5322 header block (parsed
 * into bkmail::mail_header) and the raw body octets (stored in
 * bkmail::mail_body::data). mail_body::content_type is left empty: it
 * cannot be derived from BODY[] alone.
 *
 * Handler contract (docs/usage.md §3.3):
 * `void(std::error_code, std::vector<bkmail::mail<A>, ...>)` in arrival
 * order; a tagged NO is reported as `errc::command_rejected`, a tagged
 * BAD as `errc::bad_command`.
 *
 * Engine protocol: the class implements the command-type contract of
 * `imap/operation_base.h`; see close_command.h for the member rundown.
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_FETCH_MESSAGE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_FETCH_MESSAGE_COMMAND_H_

#include <bkmail/detail/allocator_ext.h>
#include <bkmail/error.h>
#include <bkmail/imap/detail/fetch_parse.h>
#include <bkmail/imap/operation_base.h>
#include <bkmail/imap/response.h>
#include <bkmail/mail.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace bkmail::imap {

/// RFC 3501 FETCH command requesting the full message via BODY.PEEK[]
/// for a sequence set.
template <class Allocator = std::allocator<std::byte>>
class fetch_message_command {
 public:
  /// Allocator type all internal storage is rebound from.
  using allocator_type = Allocator;
  /// One assembled message per matched sequence number, in arrival order.
  using result_type =
      bkmail::detail::vector_of<bkmail::mail<Allocator>, Allocator>;
  /// Rebound character string.
  using string_type = bkmail::detail::string_of<Allocator>;

  /// Creates the command for a sequence set in RFC 3501 syntax.
  explicit fetch_message_command(std::string_view seq_set,
                                 const Allocator& alloc = Allocator{})
      : alloc_(alloc),
        seq_set_(seq_set, char_allocator{alloc}),
        line_(char_allocator{alloc}),
        messages_(mail_allocator{alloc}) {}

  /// Returns the allocator the command was created with.
  [[nodiscard]] allocator_type get_allocator() const noexcept { return alloc_; }

  /// Stamps the tag and renders `tag FETCH <seq-set> (BODY.PEEK[])\r\n`.
  void render(std::string_view tag) {
    line_.clear();
    line_.append(tag);
    line_.append(" FETCH ");
    line_.append(seq_set_);
    line_.append(" (BODY.PEEK[])\r\n");
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

  /// Absorbs `* n FETCH (BODY[] <nstring>)` responses and assembles a
  /// bkmail::mail from each; unrelated responses are ignored.
  void on_untagged(const untagged_response<Allocator>& r) {
    if (r.kind != untagged_kind::fetch) return;
    std::optional<string_type> value;
    if (!detail::parse_body_section_attribute(r.payload, "BODY[]", alloc_,
                                              value) ||
        !value) {
      return;
    }
    bkmail::mail<Allocator> message;
    const std::string_view text{*value};
    // Split header and body at the first empty line (CRLF or LF style).
    std::size_t split = text.find("\r\n\r\n");
    std::size_t skip = 4;
    if (split == std::string_view::npos) {
      split = text.find("\n\n");
      skip = 2;
    }
    const std::string_view header_block = text.substr(0, split);
    const std::string_view body_block = split == std::string_view::npos
                                            ? std::string_view{}
                                            : text.substr(split + skip);
    detail::parse_mail_header_block(header_block, alloc_, message.header);
    const auto* bytes = reinterpret_cast<const std::byte*>(body_block.data());
    message.body.data.assign(bytes, bytes + body_block.size());
    messages_.push_back(std::move(message));
  }

  /// FIFO ownership (operation_base.h): the `* n FETCH (...)` data kind.
  [[nodiscard]] static constexpr bool wants_untagged(
      untagged_kind kind) noexcept {
    return kind == untagged_kind::fetch;
  }

  /// Maps the tagged completion: OK fills `out` with the assembled
  /// messages, NO -> command_rejected, BAD -> bad_command.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    const auto ec = detail::map_status_to_ec(r.status);
    if (!ec) out = std::move(messages_);
    return ec;
  }

 private:
  using char_allocator = bkmail::detail::rebind_alloc_t<Allocator, char>;
  using mail_allocator =
      bkmail::detail::rebind_alloc_t<Allocator, bkmail::mail<Allocator>>;

  [[no_unique_address]] Allocator alloc_;
  string_type seq_set_;
  string_type line_;
  result_type messages_;
  bool segment_pending_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_FETCH_MESSAGE_COMMAND_H_
