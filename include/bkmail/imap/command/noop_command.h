/**
 * @file include/bkmail/imap/command/noop_command.h
 * @brief NOOP command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_NOOP_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_NOOP_COMMAND_H_

#include <bkmail/common/error.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// NOOP command.
///
/// One command = one C++ type. A command renders its own command line once
/// a tag is stamped and maps the tagged reply onto its `result_type`;
/// `detail::operation_model` (imap/operation_base.h) drives the type
/// through this interface:
///
/// - `render(tag)` stamps the context-allocated tag and renders every
///   byte the command will ever send (all segments included).
/// - `next_segment()` returns the currently stageable byte chunk, or an
///   empty view while the command is paused on a continuation request.
/// - `awaits_continuation()` reports the literal/SASL wall: true while a
///   continuation request is needed before more bytes may be staged.
/// - `on_segment_flushed()` advances past the last staged segment.
/// - `on_continuation(text)` releases the next segment after `+ ...`.
/// - `on_untagged(r)` lets the command absorb its untagged responses.
/// - `on_tagged(r)` maps the tagged reply: it returns the error code to
///   deliver to the handler (`errc::command_rejected` on NO,
///   `errc::bad_command` on BAD); result-bearing commands additionally
///   move the assembled result into an out-parameter that the caller
///   default-constructed and that is filled only on success.
template <class Allocator = std::allocator<std::byte>>
class noop_command {
 public:
  using allocator_type = Allocator;
  using result_type = void;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  explicit noop_command(const Allocator& alloc = Allocator{}) : wire_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" NOOP\r\n");
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    return emitted_ ? bnio::const_buffer{}
                    : bnio::const_buffer{wire_.data(), wire_.size()};
  }

  /// NOOP never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept { emitted_ = true; }

  /// NOOP expects no continuation; ignored.
  void on_continuation(std::string_view /*text*/) {}

  /// NOOP collects no untagged responses.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Advisory for the write pump: NOOP pipelines freely.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return false; }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply to the handler error code (NO -> rejected,
  /// BAD -> bad_command).
  [[nodiscard]] std::error_code on_tagged(
      const tagged_response<Allocator>& r) const {
    return status_ec(r.status);
  }

 private:
  /// Maps a tagged status onto the callback-path error code contract.
  static std::error_code status_ec(response_status status) {
    switch (status) {
      case response_status::ok:
        return {};
      case response_status::no:
        return make_error_code(errc::command_rejected);
      case response_status::bad:
        return make_error_code(errc::bad_command);
      default:
        return make_error_code(errc::unexpected_response);
    }
  }

  string_type wire_;  // Whole command line, tag included.
  bool emitted_ = false;
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_NOOP_COMMAND_H_
