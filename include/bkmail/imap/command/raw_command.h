/**
 * @file include/bkmail/imap/command/raw_command.h
 * @brief Caller-supplied raw IMAP command line.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_RAW_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_RAW_COMMAND_H_

#include <bkmail/imap/raw_response.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// Caller-supplied raw IMAP command line.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// The caller supplies the command text without tag and without CRLF; the
/// context-allocated tag is prepended at submission. Extension commands
/// that need literals are out of scope here (no continuation support):
/// use a dedicated command type or drive the exchange by hand.
///
/// Escape-hatch semantics: a tagged `NO` or `BAD` is **not** mapped onto
/// an error code. The tagged reply is reported as `raw_response` with
/// `ok == false`, so callers can interpret extension-specific outcomes
/// themselves. Untagged responses belong to the caller too — they flow
/// through `imap_context::on_unsolicited`, not through this type.
template <class Allocator = std::allocator<std::byte>>
class raw_command {
 public:
  using allocator_type = Allocator;
  using result_type = raw_response<Allocator>;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  /// Stores the command line (without tag, without CRLF).
  explicit raw_command(std::string_view line,
                       const Allocator& alloc = Allocator{})
      : line_(line, alloc), wire_(alloc), tag_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line.
  void render(std::string_view tag) {
    tag_.assign(tag);
    wire_.assign(tag);
    wire_.push_back(' ');
    wire_.append(line_);
    wire_.append("\r\n");
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    return emitted_ ? bnio::const_buffer{}
                    : bnio::const_buffer{wire_.data(), wire_.size()};
  }

  /// raw_command never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept { emitted_ = true; }

  /// raw_command expects no continuation; ignored.
  void on_continuation(std::string_view /*text*/) {}

  /// Untagged responses are the caller's business (unsolicited path).
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Advisory for the write pump: pipelining is the caller's judgement.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  // ---- reply mapping ----------------------------------------------------

  /// Always succeeds at the transport level: the tagged status rides in
  /// `raw_response::ok` (false for NO/BAD) so extension outcomes stay
  /// interpretable by the caller.
  std::error_code on_tagged(const tagged_response<Allocator>& r,
                            result_type& out) {
    out.tag = tag_;
    out.ok = r.status == response_status::ok;
    out.text = r.text;
    return {};
  }

 private:
  string_type line_;  // Caller-supplied command text.
  string_type wire_;  // Whole command line, tag included.
  string_type tag_;   // Stamped tag, echoed into the result.
  bool emitted_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_RAW_COMMAND_H_
