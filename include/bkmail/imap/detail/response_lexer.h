/**
 * @file include/bkmail/imap/detail/response_lexer.h
 * @brief Two-mode (line/literal) IMAP response framer over a dynamic buffer.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_RESPONSE_LEXER_H_
#define BKMAIL_IMAP_DETAIL_RESPONSE_LEXER_H_

#include <bkmail/imap/detail/astring.h>
#include <bnio/buffer/dynamic_byte_vector.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace bkmail::imap::detail {

/**
 * Two-mode framing cursor over the committed bytes of a
 * `bnio::dynamic_byte_vector_buffer` (RFC 3501 §4.3).
 *
 * The receiver is always in exactly one of two modes:
 *
 * - **Line mode** — scan for the next `CRLF`. If the line ends with a
 *   literal trailer (`{n}`, `{n+}`, or the obsolete `~{n}` spelling), the
 *   trailer is part of the response and the lexer switches to literal mode
 *   with `n` octets remaining.
 * - **Literal mode** — wait until `n` more bytes are committed; those bytes
 *   belong to the response verbatim and are *never scanned* for `CRLF`
 *   (a literal may contain arbitrary octets, including `CRLF`). Then the
 *   lexer returns to line mode.
 *
 * A response is complete when a line-mode line without a literal trailer
 * terminates. Server-to-client literals have no continuation handshake: the
 * `n` octets follow the marker's `CRLF` immediately.
 *
 * Usage contract with the read pump:
 *
 * 1. Call `next_complete_response(buffer)`; on success it returns a view
 *    aliasing the buffer's committed bytes (final CRLF excluded).
 * 2. Parse and dispatch the response while the view is alive.
 * 3. Call `consume_response(buffer)` to drop the response bytes in one shot
 *    (front-erase is O(n); consuming only complete responses bounds the
 *    compaction cost). Extracting the next response before consuming the
 *    previous one is a contract violation.
 */
template <class Allocator = std::allocator<std::byte>>
class response_lexer {
 public:
  /// The read buffer this lexer frames.
  using buffer_type = bnio::dynamic_byte_vector_buffer<Allocator>;

  /// Creates a lexer in line mode at the start of the buffer.
  response_lexer() noexcept = default;

  /**
   * Attempts to extract one complete response from `buffer`.
   *
   * Returns a view of the response (excluding the terminating CRLF) that
   * aliases the buffer, or `std::nullopt` when more bytes must be committed.
   * The view — and any views derived from it by the parser — is invalidated
   * by the next `consume_response()` or buffer growth.
   */
  [[nodiscard]] std::optional<std::string_view> next_complete_response(
      const buffer_type& buffer) noexcept {
    assert(response_end_ == 0 &&
           "consume_response() must precede the next extraction");
    const auto* data = static_cast<const char*>(buffer.data().data());
    const std::size_t size = buffer.data().size();

    for (;;) {
      if (mode_ == scan_mode::literal) {
        const std::size_t available = size - scan_pos_;
        if (static_cast<std::uint64_t>(available) < literal_remaining_) {
          return std::nullopt;  // Wait for the rest of the literal.
        }
        scan_pos_ += static_cast<std::size_t>(literal_remaining_);
        literal_remaining_ = 0;
        mode_ = scan_mode::line;
        line_start_ = scan_pos_;
        continue;
      }

      // Line mode: find the next CRLF at or after scan_pos_.
      std::size_t eol = size;
      for (std::size_t i = scan_pos_; i + 1 < size; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
          eol = i;
          break;
        }
      }
      if (eol == size) {
        // No complete line yet. Resume at the last committed byte next time:
        // it may be the CR of a CRLF split across two reads.
        scan_pos_ = size > line_start_ ? size - 1 : line_start_;
        return std::nullopt;
      }

      const std::string_view line(data + line_start_, eol - line_start_);
      if (const auto literal_size = literal_trailer(line)) {
        literal_remaining_ = *literal_size;
        scan_pos_ = eol + 2;  // Literal octets start right after the CRLF.
        mode_ = scan_mode::literal;
        continue;
      }

      // A plain line terminates the response.
      response_end_ = eol + 2;
      return std::string_view(data, eol);
    }
  }

  /**
   * Drops the bytes of the response most recently returned by
   * `next_complete_response()` from the front of `buffer` and resets the
   * cursor for the next response.
   */
  void consume_response(buffer_type& buffer) noexcept {
    assert(response_end_ != 0 && "no complete response to consume");
    buffer.consume(response_end_);
    reset();
  }

  /// Clears all cursor state (connection teardown / resynchronization).
  void reset() noexcept {
    mode_ = scan_mode::line;
    scan_pos_ = 0;
    line_start_ = 0;
    literal_remaining_ = 0;
    response_end_ = 0;
  }

 private:
  enum class scan_mode { line, literal };

  /**
   * Parses a literal trailer at the end of `line`: `{n}`, `{n+}`, or `~{n}`.
   * Returns the literal octet count, or `std::nullopt` when the line has no
   * valid trailer (a digit run that overflows 64 bits also yields
   * `std::nullopt`, so the line is then treated as plain text).
   */
  static std::optional<std::uint64_t> literal_trailer(
      std::string_view line) noexcept {
    if (line.size() < 3 || line.back() != '}') {
      return std::nullopt;
    }
    std::size_t end = line.size() - 1;  // Index of '}'.
    if (end > 0 && line[end - 1] == '+') {
      --end;  // LITERAL+ marker; server-to-client it changes nothing.
    }
    const std::size_t digit_end = end;
    while (end > 0 && is_digit(line[end - 1])) {
      --end;
    }
    if (end == digit_end || end == 0 || line[end - 1] != '{') {
      return std::nullopt;
    }
    const std::size_t open = end - 1;  // Index of '{'.
    // The marker must be a token of its own: preceded by SP (the usual case)
    // or by '~' (the obsolete LITERAL- spelling `~{n}`).
    if (open > 0 && line[open - 1] != ' ' && line[open - 1] != '~') {
      return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t i = end; i < digit_end; ++i) {
      const auto d = static_cast<std::uint64_t>(line[i] - '0');
      if (value > (~std::uint64_t{0} - d) / 10) {
        return std::nullopt;  // Overflow: not a plausible literal count.
      }
      value = value * 10 + d;
    }
    return value;
  }

  scan_mode mode_ = scan_mode::line;
  std::size_t scan_pos_ = 0;             ///< Line-scan resume / literal cursor.
  std::size_t line_start_ = 0;           ///< Start of the current line.
  std::uint64_t literal_remaining_ = 0;  ///< Octets left in literal mode.
  std::size_t response_end_ = 0;  ///< Past-the-end of a returned response.
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_RESPONSE_LEXER_H_
