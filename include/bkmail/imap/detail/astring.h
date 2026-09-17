/**
 * @file include/bkmail/imap/detail/astring.h
 * @brief IMAP astring/quoted/literal character classes and rendering helpers.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_ASTRING_H_
#define BKMAIL_IMAP_DETAIL_ASTRING_H_

#include <cassert>
#include <charconv>
#include <cstddef>
#include <string_view>

namespace bkmail::imap::detail {

/// CHAR (RFC 3501 §9): any 7-bit character except NUL.
[[nodiscard]] constexpr bool is_char(char c) noexcept {
  const auto u = static_cast<unsigned char>(c);
  return u >= 0x01 && u <= 0x7f;
}

/// CTL: %x00-1F / %x7F.
[[nodiscard]] constexpr bool is_ctl(char c) noexcept {
  const auto u = static_cast<unsigned char>(c);
  return u <= 0x1f || u == 0x7f;
}

/// list-wildcards: "%" / "*".
[[nodiscard]] constexpr bool is_list_wildcard(char c) noexcept {
  return c == '%' || c == '*';
}

/// quoted-specials: DQUOTE / "\".
[[nodiscard]] constexpr bool is_quoted_special(char c) noexcept {
  return c == '"' || c == '\\';
}

/// resp-specials: "]".
[[nodiscard]] constexpr bool is_resp_special(char c) noexcept {
  return c == ']';
}

/// atom-specials: "(" / ")" / "{" / SP / CTL / list-wildcards /
/// quoted-specials / resp-specials.
[[nodiscard]] constexpr bool is_atom_special(char c) noexcept {
  return c == '(' || c == ')' || c == '{' || c == ' ' || is_ctl(c) ||
         is_list_wildcard(c) || is_quoted_special(c) || is_resp_special(c);
}

/// ATOM-CHAR: any CHAR except atom-specials.
[[nodiscard]] constexpr bool is_atom_char(char c) noexcept {
  return is_char(c) && !is_atom_special(c);
}

/// ASTRING-CHAR: ATOM-CHAR / resp-specials.
[[nodiscard]] constexpr bool is_astring_char(char c) noexcept {
  return is_atom_char(c) || is_resp_special(c);
}

/// Tag characters are ASTRING-CHAR with "+" excluded (RFC 3501 §9).
[[nodiscard]] constexpr bool is_tag_char(char c) noexcept {
  return is_astring_char(c) && c != '+';
}

/// TEXT-CHAR: any CHAR except CR and LF.
[[nodiscard]] constexpr bool is_text_char(char c) noexcept {
  return is_char(c) && c != '\r' && c != '\n';
}

/// DIGIT: %x30-39.
[[nodiscard]] constexpr bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

/// ASCII-only uppercase mapping, for case-insensitive protocol keywords.
[[nodiscard]] constexpr char ascii_to_upper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// Case-insensitive ASCII equality of two keywords/atoms.
[[nodiscard]] constexpr bool ascii_iequals(std::string_view a,
                                           std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_to_upper(a[i]) != ascii_to_upper(b[i])) {
      return false;
    }
  }
  return true;
}

/// How an astring argument must be rendered on the wire.
enum class astring_class {
  atom,    ///< every character is an ASTRING-CHAR; send verbatim.
  quoted,  ///< contains specials but no CR/LF or 8-bit; send quoted.
  literal  ///< contains CR/LF or 8-bit data; must be sent as a literal.
};

/// Classifies `value` for astring rendering. The empty string classifies as
/// `quoted` (`""`), because a bare atom must be non-empty.
[[nodiscard]] constexpr astring_class classify_astring(
    std::string_view value) noexcept {
  if (value.empty()) {
    return astring_class::quoted;
  }
  bool atom = true;
  bool text = true;
  for (const char c : value) {
    atom = atom && is_astring_char(c);
    text = text && is_text_char(c);
  }
  if (atom) {
    return astring_class::atom;
  }
  return text ? astring_class::quoted : astring_class::literal;
}

/**
 * Appends `value` as a quoted string to `out`.
 *
 * Only `\"` and `\\` are escaped (RFC 3501 §9 quoted-specials). Precondition:
 * every character of `value` is a TEXT-CHAR (no CR/LF, no 8-bit); values that
 * violate this must go through `render_literal` instead. `String` is any
 * appendable character container (`push_back(char)` / `append(const char*,
 * size)`).
 */
template <class String>
void render_quoted(std::string_view value, String& out) {
  out.push_back('"');
  for (const char c : value) {
    assert(is_text_char(c) && "render_quoted: value requires a literal");
    if (is_quoted_special(c)) {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
}

/**
 * Appends `value` as an astring to `out`: verbatim when every character is an
 * ASTRING-CHAR, quoted otherwise. Precondition: `classify_astring(value)` is
 * not `literal`; the caller must route literal-class values through
 * `render_literal` because sending a literal changes the command framing
 * (continuation handshake) and cannot be decided here.
 */
template <class String>
void render_astring(std::string_view value, String& out) {
  const astring_class cls = classify_astring(value);
  assert(cls != astring_class::literal &&
         "render_astring: value requires a literal");
  if (cls == astring_class::atom) {
    out.append(value.data(), value.size());
  } else {
    render_quoted(value, out);
  }
}

/// Literal framing flavour (RFC 3501 §4.3 / RFC 2088 LITERAL+).
enum class literal_mode {
  synchronizing,     ///< `{n}` — sender must wait for a continuation request.
  non_synchronizing  ///< `{n+}` — LITERAL+; no continuation wait.
};

/**
 * Appends a literal segment to `out`: the `{n}` / `{n+}` marker, CRLF, then
 * the literal octets verbatim. The synchronizing marker forms a hard write
 * wall; detecting it before flushing is the write pump's job.
 */
template <class String>
void render_literal(std::string_view data, String& out,
                    literal_mode mode = literal_mode::synchronizing) {
  char digits[32];
  const auto conv = std::to_chars(digits, digits + sizeof(digits), data.size());
  out.push_back('{');
  out.append(digits, static_cast<std::size_t>(conv.ptr - digits));
  if (mode == literal_mode::non_synchronizing) {
    out.push_back('+');
  }
  out.append("}\r\n", 3);
  out.append(data.data(), data.size());
}

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_ASTRING_H_
