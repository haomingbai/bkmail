/**
 * @file include/bkmail/common/mail_header.h
 * @brief Structured RFC 5322 header view with MIME decoding.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_COMMON_MAIL_HEADER_H_
#define BKMAIL_COMMON_MAIL_HEADER_H_

#include <bkmail/common/address.h>
#include <bkmail/common/detail/allocator_ext.h>

#include <cctype>
#include <cstddef>
#include <memory>
#include <string_view>

namespace bkmail::detail {

/// Returns the 6-bit value of a Base64 alphabet character, -2 for the
/// padding character `=`, or -1 when `c` is outside the alphabet.
inline int mime_base64_value(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return -2;
  return -1;
}

/// Returns the value of a hexadecimal digit, or -1 when `c` is not one.
inline int mime_hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/**
 * @brief Decodes one RFC 2047 encoded-word at `pos`, appending the decoded
 * bytes to `out`.
 * @return The index just past the word, or `pos` when the text at `pos` is
 * not a well-formed encoded-word (`out` is then left untouched).
 */
template <class String>
[[nodiscard]] std::size_t decode_one_encoded_word(std::string_view text,
                                                  std::size_t pos,
                                                  String& out) {
  const std::size_t n = text.size();
  if (pos + 1 >= n || text[pos] != '=' || text[pos + 1] != '?') return pos;

  const std::size_t charset_begin = pos + 2;
  const std::size_t charset_end = text.find('?', charset_begin);
  if (charset_end == std::string_view::npos || charset_end == charset_begin) {
    return pos;
  }
  for (std::size_t k = charset_begin; k < charset_end; ++k) {
    if (text[k] == ' ' || text[k] == '\t') return pos;
  }

  const std::size_t encoding_pos = charset_end + 1;
  if (encoding_pos + 1 >= n || text[encoding_pos + 1] != '?') return pos;
  const char encoding = static_cast<char>(
      std::toupper(static_cast<unsigned char>(text[encoding_pos])));
  if (encoding != 'B' && encoding != 'Q') return pos;

  const std::size_t data_begin = encoding_pos + 2;
  const std::size_t data_end = text.find("?=", data_begin);
  if (data_end == std::string_view::npos) return pos;
  const std::string_view data = text.substr(data_begin, data_end - data_begin);

  // Decode into scratch first so a malformed word leaves `out` untouched.
  String scratch(out.get_allocator());
  if (encoding == 'B') {
    if (data.size() % 4 != 0) return pos;
    for (std::size_t k = 0; k < data.size(); k += 4) {
      int v[4];
      for (std::size_t j = 0; j < 4; ++j) {
        v[j] = mime_base64_value(data[k + j]);
        if (v[j] == -1) return pos;
      }
      const bool last_group = (k + 4 == data.size());
      if (v[0] < 0 || v[1] < 0) return pos;
      if (!last_group && (v[2] < 0 || v[3] < 0)) return pos;
      if (v[2] == -2 && v[3] != -2) return pos;
      scratch.push_back(static_cast<char>((v[0] << 2) | (v[1] >> 4)));
      if (v[2] >= 0) {
        scratch.push_back(static_cast<char>(((v[1] & 0xF) << 4) | (v[2] >> 2)));
      }
      if (v[3] >= 0) {
        scratch.push_back(static_cast<char>(((v[2] & 0x3) << 6) | v[3]));
      }
    }
  } else {
    for (std::size_t k = 0; k < data.size(); ++k) {
      const char c = data[k];
      if (c == '_') {
        scratch.push_back(' ');
        continue;
      }
      if (c == '=') {
        if (k + 2 >= data.size()) return pos;
        const int hi = mime_hex_value(data[k + 1]);
        const int lo = mime_hex_value(data[k + 2]);
        if (hi < 0 || lo < 0) return pos;
        scratch.push_back(static_cast<char>((hi << 4) | lo));
        k += 2;
        continue;
      }
      scratch.push_back(c);
    }
  }

  out += scratch;
  return data_end + 2;
}

/**
 * @brief Decodes every RFC 2047 encoded-word in `text`.
 *
 * Text outside encoded-words passes through unchanged; linear whitespace
 * between two adjacent encoded-words is dropped (RFC 2047 §6.2). The
 * decoded bytes are delivered in each word's declared charset — no charset
 * transcoding is performed (callers needing UTF-8 must convert).
 */
template <class Allocator = std::allocator<std::byte>>
[[nodiscard]] string_of<Allocator> decode_encoded_words(
    std::string_view text, const Allocator& alloc = Allocator{}) {
  using string_type = string_of<Allocator>;
  string_type out{rebind_alloc_t<Allocator, char>(alloc)};
  out.reserve(text.size());

  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    const std::size_t next = decode_one_encoded_word(text, i, out);
    if (next == i) {
      out.push_back(text[i]);
      ++i;
      continue;
    }
    i = next;
    for (;;) {
      std::size_t j = i;
      while (j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' ||
                       text[j] == '\n')) {
        ++j;
      }
      if (j == i || j + 1 >= n || text[j] != '=' || text[j + 1] != '?') break;
      string_type scratch(out.get_allocator());
      const std::size_t word_end = decode_one_encoded_word(text, j, scratch);
      if (word_end == j) break;
      out += scratch;
      i = word_end;
    }
  }
  return out;
}

}  // namespace bkmail::detail

namespace bkmail {

/**
 * @brief Structured RFC 5322 header view.
 *
 * Produced by `BODY.PEEK[HEADER.FIELDS (...)]` fetches. `date` is kept
 * unparsed; address fields are address lists. MIME semantics (RFC 2047
 * encoded-word decoding) live in `bkmail::detail::decode_encoded_words`,
 * declared in this header, never in the protocol parser.
 */
template <class Allocator = std::allocator<std::byte>>
struct mail_header {
  using allocator_type = Allocator;
  using string_type = detail::string_of<Allocator>;

  /// Address-list container type used by this header.
  using address_list = detail::vector_of<address<Allocator>, Allocator>;

  /// Subject field (decoded on demand; stored as received).
  string_type subject;
  /// From address list.
  address_list from;
  /// To address list.
  address_list to;
  /// Cc address list.
  address_list cc;
  /// Bcc address list.
  address_list bcc;
  /// Date field, unparsed.
  string_type date;
  /// Message-ID field.
  string_type message_id;
  /// In-Reply-To field.
  string_type in_reply_to;
};

}  // namespace bkmail

#endif  // BKMAIL_COMMON_MAIL_HEADER_H_
