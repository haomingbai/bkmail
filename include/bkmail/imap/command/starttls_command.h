/**
 * @file include/bkmail/imap/command/starttls_command.h
 * @brief STARTTLS command.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_STARTTLS_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_STARTTLS_COMMAND_H_

#include <bkmail/error.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace bkmail::imap {

/// STARTTLS command.
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// On a tagged OK the transport upgrade itself (moving the socket into an
/// ssl_stream and handshaking) is the caller's business; this type only
/// carries the negotiation. `blocks_pipeline()` is set: nothing may be
/// staged behind STARTTLS on the plaintext connection.
template <class Allocator = std::allocator<std::byte>>
class starttls_command {
 public:
  using allocator_type = Allocator;
  using result_type = void;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  explicit starttls_command(const Allocator& alloc = Allocator{})
      : wire_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line.
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" STARTTLS\r\n");
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    return emitted_ ? bnio::const_buffer{}
                    : bnio::const_buffer{wire_.data(), wire_.size()};
  }

  /// STARTTLS never waits for a continuation request.
  [[nodiscard]] bool awaits_continuation() const noexcept { return false; }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept { emitted_ = true; }

  /// STARTTLS expects no continuation; ignored.
  void on_continuation(std::string_view /*text*/) {}

  /// STARTTLS collects no untagged responses.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Advisory for the write pump: the TLS upgrade forbids pipelining.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return true; }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply to the handler error code.
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

#endif  // BKMAIL_IMAP_COMMAND_STARTTLS_COMMAND_H_
