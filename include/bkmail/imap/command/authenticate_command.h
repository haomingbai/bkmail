/**
 * @file include/bkmail/imap/command/authenticate_command.h
 * @brief AUTHENTICATE command (SASL, SASL-IR aware).
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_COMMAND_AUTHENTICATE_COMMAND_H_
#define BKMAIL_IMAP_COMMAND_AUTHENTICATE_COMMAND_H_

#include <bkmail/error.h>
#include <bkmail/imap/response.h>
#include <bnio/buffer/basic.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace bkmail::imap {

/// AUTHENTICATE command (SASL, SASL-IR aware).
///
/// One command = one C++ type; `detail::operation_model`
/// (imap/operation_base.h) drives the type through the command interface
/// documented on `noop_command` (render / next_segment /
/// awaits_continuation / on_segment_flushed / on_continuation /
/// on_untagged / on_tagged).
///
/// The command carries one SASL response payload (for PLAIN this is the
/// `authzid \0 authcid \0 passwd` blob built by the caller):
///
/// - With the server capability SASL-IR (@p sasl_ir = true) the payload
///   rides on the command line as the initial response:
///   `AUTHENTICATE mech base64(payload)` — one round trip, no wall.
/// - Otherwise the command waits for the (for PLAIN empty) `+` challenge
///   and answers with `base64(payload)`; an empty payload is answered
///   with `*`, the SASL cancellation line (the server then replies with a
///   tagged BAD, reported as `errc::bad_command`).
/// - A further challenge after the payload was sent (multi-step
///   mechanisms are not modelled) is likewise answered with `*`.
template <class Allocator = std::allocator<std::byte>>
class authenticate_command {
 public:
  using allocator_type = Allocator;
  using result_type = void;
  using string_type = std::basic_string<
      char, std::char_traits<char>,
      typename std::allocator_traits<Allocator>::template rebind_alloc<char>>;

  /// Stores the mechanism name and the (not yet base64-encoded) SASL
  /// response payload. @p sasl_ir mirrors the server's SASL-IR capability.
  authenticate_command(std::string_view mechanism, std::string_view response,
                       bool sasl_ir = false,
                       const Allocator& alloc = Allocator{})
      : mechanism_(mechanism, alloc),
        response_(response, alloc),
        sasl_ir_(sasl_ir),
        wire_(alloc) {}

  // ---- wire production (driven by detail::operation_model) -------------

  /// Stamps @p tag and renders the command line (and, without SASL-IR,
  /// the challenge answer as a walled-off second segment).
  void render(std::string_view tag) {
    wire_.assign(tag);
    wire_.append(" AUTHENTICATE ");
    wire_.append(mechanism_);
    if (sasl_ir_) {
      wire_.push_back(' ');
      if (response_.empty()) {
        wire_.push_back('=');  // SASL-IR empty initial response
      } else {
        append_base64(response_);
      }
      wire_.append("\r\n");
      seg_[0] = {0, wire_.size()};
      seg_count_ = 1;
      unlocked_ = 1;
      return;
    }
    wire_.append("\r\n");
    seg_[0] = {0, wire_.size()};
    const std::size_t seg_begin = wire_.size();
    if (response_.empty()) {
      wire_.append("*\r\n");  // No payload: cancel once challenged.
    } else {
      append_base64(response_);
      wire_.append("\r\n");
    }
    seg_[1] = {seg_begin, wire_.size()};
    seg_count_ = 2;
    unlocked_ = 1;  // Segment 1 is released by the '+' challenge.
  }

  /// Returns the byte chunk that may be staged now, or an empty view.
  [[nodiscard]] bnio::const_buffer next_segment() const noexcept {
    if (emitted_ >= unlocked_ || emitted_ >= seg_count_) return {};
    const auto [off, end] = seg_[emitted_];
    return bnio::const_buffer{wire_.data() + off, end - off};
  }

  /// True while the SASL wall blocks the challenge answer.
  [[nodiscard]] bool awaits_continuation() const noexcept {
    return unlocked_ < seg_count_;
  }

  /// Marks the staged segment as written.
  void on_segment_flushed() noexcept {
    if (emitted_ < seg_count_) ++emitted_;
  }

  /// Releases the challenge answer; unexpected extra challenges (multi-step
  /// mechanisms are not modelled) are answered with the `*` cancel line.
  void on_continuation(std::string_view /*text*/) {
    if (unlocked_ < seg_count_) {
      ++unlocked_;
      return;
    }
    if (seg_count_ >= seg_.size()) return;
    const std::size_t off = wire_.size();
    wire_.append("*\r\n");
    seg_[seg_count_++] = {off, wire_.size()};
    ++unlocked_;
  }

  /// AUTHENTICATE collects no untagged responses.
  void on_untagged(const untagged_response<Allocator>& /*r*/) {}

  /// Advisory for the write pump: SASL exchanges must not be pipelined.
  [[nodiscard]] bool blocks_pipeline() const noexcept { return true; }

  // ---- reply mapping ----------------------------------------------------

  /// Maps the tagged reply to the handler error code (NO -> rejected,
  /// BAD -> bad_command, e.g. after a `*` cancel).
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

  /// Appends @p in base64-encoded (RFC 4648, with padding).
  void append_base64(std::string_view in) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
      const auto n =
          (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i]))
           << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1]))
           << 8) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 2]));
      wire_.push_back(kAlphabet[(n >> 18) & 63]);
      wire_.push_back(kAlphabet[(n >> 12) & 63]);
      wire_.push_back(kAlphabet[(n >> 6) & 63]);
      wire_.push_back(kAlphabet[n & 63]);
      i += 3;
    }
    const auto rem = in.size() - i;
    if (rem == 1) {
      const auto n =
          static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16;
      wire_.push_back(kAlphabet[(n >> 18) & 63]);
      wire_.push_back(kAlphabet[(n >> 12) & 63]);
      wire_.append("==");
    } else if (rem == 2) {
      const auto n =
          (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i]))
           << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1]))
           << 8);
      wire_.push_back(kAlphabet[(n >> 18) & 63]);
      wire_.push_back(kAlphabet[(n >> 12) & 63]);
      wire_.push_back(kAlphabet[(n >> 6) & 63]);
      wire_.push_back('=');
    }
  }

  string_type mechanism_;
  string_type response_;
  bool sasl_ir_ = false;
  string_type wire_;  // All segments, concatenated.
  std::array<std::pair<std::size_t, std::size_t>, 4> seg_{};
  std::size_t seg_count_ = 0;
  std::size_t emitted_ = 0;   // Segments handed to the write pump.
  std::size_t unlocked_ = 0;  // Segments released by challenges so far.
};

}  // namespace bkmail::imap

#endif  // BKMAIL_IMAP_COMMAND_AUTHENTICATE_COMMAND_H_
