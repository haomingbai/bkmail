/**
 * @file include/bkmail/imap/detail/fetch_parse.h
 * @brief Deep parsers for FETCH response payloads (ENVELOPE, BODYSTRUCTURE,
 *        attribute lists, RFC 5322 header blocks).
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 *
 * @details
 * Shared by every FETCH-family command (fetch_envelopes_command,
 * fetch_headers_command, fetch_message_command, fetch_command and the four
 * uid_ variants), which is the "two or more fetch commands" evidence that
 * docs/code_layout.md §4 requires for this header to exist.
 *
 * All parsers run on `detail::parse_cursor` (detail/response_parser.h) over
 * the dispatch-lifetime `untagged_response::payload` view: literals are
 * already stitched into the response buffer by the lexer, and the cursor's
 * string readers return views into it that are copied into the result
 * containers before dispatch ends.
 *
 * NIL policy: wire NIL maps to empty strings / empty vectors, matching the
 * binding decision D8 of docs/code_layout.md (NIL is only semantically
 * distinct on mailbox_info::unseen).
 *
 * The parsers are fault-tolerant: extension data they do not model is
 * skipped with a balanced s-expression skipper instead of failing the whole
 * FETCH response.
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_FETCH_PARSE_H_
#define BKMAIL_IMAP_DETAIL_FETCH_PARSE_H_

#include <bkmail/common/address.h>
#include <bkmail/common/body_structure.h>
#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/envelope.h>
#include <bkmail/common/mail_header.h>
#include <bkmail/imap/detail/astring.h>
#include <bkmail/imap/detail/response_parser.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/message_attributes.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace bkmail::imap::detail {

/// Consumes a `NIL` atom with an atom-boundary check (so `NILO` is not
/// mistaken for NIL). Returns false when the input does not start with NIL.
template <class Allocator>
bool consume_nil(parse_cursor<Allocator>& cur) noexcept {
  const std::string_view rest = cur.remaining();
  if (rest.size() < 3 || !ascii_iequals(rest.substr(0, 3), "NIL")) {
    return false;
  }
  if (rest.size() > 3 && is_atom_char(rest[3])) {
    return false;
  }
  return cur.try_consume_ci("NIL");
}

/// Reads an nstring into `member`: NIL leaves the (empty) member untouched
/// (decision D8), a value is copied in. Returns false on malformed input.
template <class Allocator>
bool read_nstr(parse_cursor<Allocator>& cur,
               bkmail::detail::string_of<Allocator>& member) {
  const auto res = cur.read_nstring();
  if (!res) return false;          // Malformed.
  if (*res) member.assign(**res);  // NIL keeps the member empty.
  return true;
}

/// Skips one arbitrary s-expression value (number, atom, NIL, quoted,
/// literal, or a balanced parenthesized list). Used to drop extension data.
template <class Allocator>
bool skip_value(parse_cursor<Allocator>& cur) {
  const char c = cur.peek();
  if (c == '(') {
    cur.try_consume('(');
    for (;;) {
      cur.skip_spaces();
      if (cur.try_consume(')')) return true;
      if (cur.at_end()) return false;
      if (!skip_value(cur)) return false;
    }
  }
  if (c == '"') return cur.read_quoted().has_value();
  if (c == '{' || c == '~') return cur.read_literal().has_value();
  if (cur.at_end()) return false;
  return !cur.read_atom().empty();
}

/// Parses a parenthesized flag list (`(\Seen \Draft)`) into `out`.
/// Unknown flags (keywords) are accepted and dropped.
template <class Allocator>
bool parse_flag_list(parse_cursor<Allocator>& cur, flag_set& out) {
  if (!cur.try_consume('(')) return false;
  for (;;) {
    cur.skip_spaces();
    if (cur.try_consume(')')) return true;
    if (cur.at_end()) return false;
    const std::string_view name = cur.read_until_any(" ()");
    if (name.empty()) return false;
    if (ascii_iequals(name, "\\Seen")) {
      out.set(message_flag::seen);
    } else if (ascii_iequals(name, "\\Answered")) {
      out.set(message_flag::answered);
    } else if (ascii_iequals(name, "\\Flagged")) {
      out.set(message_flag::flagged);
    } else if (ascii_iequals(name, "\\Deleted")) {
      out.set(message_flag::deleted);
    } else if (ascii_iequals(name, "\\Draft")) {
      out.set(message_flag::draft);
    } else if (ascii_iequals(name, "\\Recent")) {
      out.set(message_flag::recent);
    }
  }
}

/// Parses one ENVELOPE address list: `NIL` (empty list) or
/// `(addr addr ...)` where each addr is `(name adl mailbox host)`.
/// NIL members of an address tuple become empty strings (decision D8).
template <class Allocator, class AddressVector>
bool parse_address_list(parse_cursor<Allocator>& cur, AddressVector& out) {
  if (consume_nil(cur)) return true;  // NIL -> empty vector.
  if (!cur.try_consume('(')) return false;
  for (;;) {
    cur.skip_spaces();
    if (cur.try_consume(')')) return true;
    if (cur.at_end()) return false;
    if (!cur.try_consume('(')) return false;
    out.emplace_back();
    auto& addr = out.back();
    if (!read_nstr(cur, addr.display_name)) return false;
    cur.skip_spaces();
    if (!read_nstr(cur, addr.adl)) return false;
    cur.skip_spaces();
    if (!read_nstr(cur, addr.mailbox_name)) return false;
    cur.skip_spaces();
    if (!read_nstr(cur, addr.host_name)) return false;
    if (!cur.try_consume(')')) return false;
  }
}

/// Parses the ten-field ENVELOPE structure into `out`.
template <class Allocator>
bool parse_envelope(parse_cursor<Allocator>& cur,
                    bkmail::envelope<Allocator>& out) {
  if (!cur.try_consume('(')) return false;
  if (!read_nstr(cur, out.date)) return false;
  cur.skip_spaces();
  if (!read_nstr(cur, out.subject)) return false;
  cur.skip_spaces();
  if (!parse_address_list(cur, out.from)) return false;
  cur.skip_spaces();
  if (!parse_address_list(cur, out.sender)) return false;
  cur.skip_spaces();
  if (!parse_address_list(cur, out.reply_to)) return false;
  cur.skip_spaces();
  if (!parse_address_list(cur, out.to)) return false;
  cur.skip_spaces();
  if (!parse_address_list(cur, out.cc)) return false;
  cur.skip_spaces();
  if (!parse_address_list(cur, out.bcc)) return false;
  cur.skip_spaces();
  if (!read_nstr(cur, out.in_reply_to)) return false;
  cur.skip_spaces();
  if (!read_nstr(cur, out.message_id)) return false;
  return cur.try_consume(')');
}

/// Parses a body-fld-param list: `("name" "value" ...)` or `NIL`.
template <class Allocator>
bool parse_body_params(parse_cursor<Allocator>& cur,
                       bkmail::body_structure<Allocator>& out) {
  using string_type = typename bkmail::body_structure<Allocator>::string_type;
  if (consume_nil(cur)) return true;
  if (!cur.try_consume('(')) return false;
  for (;;) {
    cur.skip_spaces();
    if (cur.try_consume(')')) return true;
    if (cur.at_end()) return false;
    const auto name = cur.read_astring();
    if (!name) return false;
    cur.skip_spaces();
    const auto value = cur.read_astring();
    if (!value) return false;
    string_type n;
    n.assign(*name);
    string_type v;
    v.assign(*value);
    out.parameters.emplace_back(std::move(n), std::move(v));
  }
}

/// Recursion bound for parse_body_structure (hostile-input guard).
inline constexpr unsigned kMaxBodyStructureDepth = 32;

/// Recursively parses a BODYSTRUCTURE node.
///
/// Basic parts consume the seven body-fld-basic fields (media type, subtype,
/// parameters, id, description, encoding, octets); the TEXT line count and
/// every extension datum after them (md5, disposition, language, location,
/// and for message/rfc822 the embedded envelope/structure) are skipped with
/// the tolerant skipper. Multipart parts recurse into `parts` and then
/// consume the subtype plus the same skippable extension tail.
template <class Allocator>
bool parse_body_structure(parse_cursor<Allocator>& cur,
                          bkmail::body_structure<Allocator>& out,
                          unsigned depth = 0) {
  if (depth > kMaxBodyStructureDepth) return false;
  if (!cur.try_consume('(')) return false;
  cur.skip_spaces();
  if (cur.peek() == '(') {
    // Multipart: one or more child parts, then the media subtype.
    out.media_type.assign("multipart");
    while (cur.peek() == '(') {
      out.parts.emplace_back();
      if (!parse_body_structure(cur, out.parts.back(), depth + 1)) {
        return false;
      }
      cur.skip_spaces();
    }
    const auto subtype = cur.read_astring();
    if (!subtype) return false;
    out.subtype.assign(*subtype);
    cur.skip_spaces();
    // Extension tail (params, disposition, language, location): skip.
    while (!cur.at_end() && cur.peek() != ')') {
      if (!skip_value(cur)) return false;
      cur.skip_spaces();
    }
    return cur.try_consume(')');
  }
  // Basic part: type subtype params id description encoding octets.
  const auto type = cur.read_astring();
  if (!type) return false;
  out.media_type.assign(*type);
  cur.skip_spaces();
  const auto subtype = cur.read_astring();
  if (!subtype) return false;
  out.subtype.assign(*subtype);
  cur.skip_spaces();
  if (!parse_body_params(cur, out)) return false;
  cur.skip_spaces();
  if (!read_nstr(cur, out.id)) return false;
  cur.skip_spaces();
  if (!read_nstr(cur, out.description)) return false;
  cur.skip_spaces();
  const auto encoding = cur.read_astring();
  if (!encoding) return false;
  out.encoding.assign(*encoding);
  cur.skip_spaces();
  const auto octets = cur.read_number();
  if (!octets) return false;
  out.octets = *octets;
  cur.skip_spaces();
  // TEXT line count, embedded message data, and the extension tail are
  // all consumed with the tolerant skipper.
  while (!cur.at_end() && cur.peek() != ')') {
    if (!skip_value(cur)) return false;
    cur.skip_spaces();
  }
  return cur.try_consume(')');
}

/// Walks a FETCH attribute list `(KEY value KEY value ...)`, invoking
/// `sink(cursor, key)` for each pair. The sink receives a fresh
/// `parse_cursor` positioned at the value and must consume exactly the
/// value (use skip_value for unknown keys); walking resumes at the
/// cursor's remainder. Keys are the raw attribute names, e.g. `FLAGS`,
/// `UID`, `ENVELOPE`, `BODY[...]`.
template <class Allocator, class Sink>
bool walk_fetch_attributes(std::string_view attr_list, const Allocator& alloc,
                           Sink&& sink) {
  std::string_view rest = attr_list;
  if (rest.empty() || rest.front() != '(') return false;
  rest.remove_prefix(1);
  for (;;) {
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (!rest.empty() && rest.front() == ')') return true;
    if (rest.empty()) return false;
    // The key may carry a section spec (`BODY[HEADER.FIELDS (...)]`) that
    // itself contains spaces; read until a space at bracket depth zero.
    std::size_t end = 0;
    unsigned brackets = 0;
    while (end < rest.size()) {
      const char c = rest[end];
      if (c == '[') ++brackets;
      if (c == ']' && brackets > 0) --brackets;
      if ((c == ' ' || c == ')') && brackets == 0) break;
      ++end;
    }
    if (end == 0) return false;
    const std::string_view key = rest.substr(0, end);
    rest.remove_prefix(end);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    parse_cursor<Allocator> cur{rest, alloc};
    if (!sink(cur, key)) return false;
    rest = cur.remaining();
  }
}

/// Finds the ENVELOPE attribute in a FETCH attribute list and deep-parses
/// it into `out`. Returns false when no well-formed ENVELOPE is present.
template <class Allocator>
bool parse_envelope_attribute(std::string_view attr_list,
                              const Allocator& alloc,
                              bkmail::envelope<Allocator>& out) {
  bool found = false;
  const bool walked = walk_fetch_attributes(
      attr_list, alloc,
      [&](parse_cursor<Allocator>& cur, std::string_view key) {
        if (ascii_iequals(key, "ENVELOPE")) {
          found = true;
          return parse_envelope(cur, out);
        }
        return skip_value(cur);
      });
  return walked && found;
}

/// Finds a BODY[...] section attribute whose key starts with
/// `section_prefix` (e.g. `BODY[]` or `BODY[HEADER.FIELDS`) and reads its
/// nstring value (quoted or literal). Returns false when the section is
/// absent or its value malformed; a NIL value yields `!out` (the D8
/// empty-string policy makes NIL indistinguishable from absent here).
template <class Allocator>
bool parse_body_section_attribute(
    std::string_view attr_list, std::string_view section_prefix,
    const Allocator& alloc,
    std::optional<bkmail::detail::string_of<Allocator>>& out) {
  using string_type = bkmail::detail::string_of<Allocator>;
  bool found = false;
  const bool walked = walk_fetch_attributes(
      attr_list, alloc,
      [&](parse_cursor<Allocator>& cur, std::string_view key) {
        if (key.size() >= section_prefix.size() &&
            ascii_iequals(key.substr(0, section_prefix.size()),
                          section_prefix)) {
          found = true;
          const auto res = cur.read_nstring();
          if (!res) return false;
          if (*res) out = string_type(**res, alloc);
          return true;
        }
        return skip_value(cur);
      });
  return walked && found;
}

/// Parses every recognized attribute of a FETCH response into
/// `message_attributes`. `BODY[...]` section payloads are stored on
/// `sections` (specifier without the `BODY[` / `]` framing, empty for
/// `BODY[]`); RFC822 / RFC822.HEADER / RFC822.TEXT and unknown attributes
/// are consumed and dropped.
template <class Allocator>
bool parse_fetch_attributes(std::string_view attr_list, const Allocator& alloc,
                            message_attributes<Allocator>& out) {
  using string_type = bkmail::detail::string_of<Allocator>;
  return walk_fetch_attributes(
      attr_list, alloc,
      [&](parse_cursor<Allocator>& cur, std::string_view key) {
        if (key.size() > 6 && ascii_iequals(key.substr(0, 5), "BODY[") &&
            key.back() == ']') {
          const auto res = cur.read_nstring();
          if (!res) return false;
          out.sections.emplace_back();
          auto& section = out.sections.back();
          section.specifier = string_type(key.substr(5, key.size() - 6), alloc);
          if (*res) {
            const std::string_view bytes = **res;
            const auto* first =
                reinterpret_cast<const std::byte*>(bytes.data());
            section.data.assign(first, first + bytes.size());
          }
          return true;
        }
        if (ascii_iequals(key, "FLAGS")) {
          return parse_flag_list(cur, out.flags);
        }
        if (ascii_iequals(key, "UID")) {
          const auto uid = cur.read_number();
          if (!uid) return false;
          out.uid = static_cast<std::uint32_t>(*uid);
          return true;
        }
        if (ascii_iequals(key, "INTERNALDATE")) {
          const auto value = cur.read_astring();
          if (!value) return false;
          out.internal_date.assign(*value);
          return true;
        }
        if (ascii_iequals(key, "RFC822.SIZE")) {
          const auto size = cur.read_number();
          if (!size) return false;
          out.rfc822_size = *size;
          return true;
        }
        if (ascii_iequals(key, "ENVELOPE")) {
          bkmail::envelope<Allocator> env;
          if (!parse_envelope(cur, env)) return false;
          out.envelope = std::move(env);
          return true;
        }
        if (ascii_iequals(key, "BODYSTRUCTURE")) {
          bkmail::body_structure<Allocator> node;
          if (!parse_body_structure(cur, node)) return false;
          out.body_structure = std::move(node);
          return true;
        }
        // RFC822*, unknown attributes: consume and drop.
        return skip_value(cur);
      });
}

/// Simplified RFC 5322 address-list parser: splits on top-level commas,
/// extracts `display <local@host>` forms, and fills address tuples.
/// Group syntax and obsolete route addresses are not modelled; they round
/// through as display-less addresses with the raw text as mailbox_name.
template <class Allocator, class AddressVector>
void parse_rfc5322_addresses(std::string_view text, AddressVector& out) {
  auto trim = [](std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' ||
                          v.front() == '\r' || v.front() == '\n')) {
      v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' ||
                          v.back() == '\r' || v.back() == '\n')) {
      v.remove_suffix(1);
    }
    return v;
  };

  std::size_t pos = 0;
  while (pos <= text.size()) {
    // Find the next comma outside quotes / angle brackets.
    std::size_t end = pos;
    bool in_quote = false;
    unsigned angles = 0;
    while (end < text.size()) {
      const char c = text[end];
      if (c == '"') in_quote = !in_quote;
      if (!in_quote && c == '<') ++angles;
      if (!in_quote && c == '>' && angles > 0) --angles;
      if (!in_quote && angles == 0 && c == ',') break;
      ++end;
    }
    const std::string_view item = trim(text.substr(pos, end - pos));
    pos = end >= text.size() ? text.size() + 1 : end + 1;
    if (item.empty()) {
      if (end >= text.size()) break;
      continue;
    }

    out.emplace_back();
    auto& addr = out.back();
    std::string_view spec = item;
    const std::size_t open = item.find('<');
    const std::size_t close = item.rfind('>');
    if (open != std::string_view::npos && close != std::string_view::npos &&
        close > open) {
      std::string_view display = trim(item.substr(0, open));
      if (display.size() >= 2 && display.front() == '"' &&
          display.back() == '"') {
        display = display.substr(1, display.size() - 2);
      }
      addr.display_name.assign(display);
      spec = item.substr(open + 1, close - open - 1);
    }
    spec = trim(spec);
    const std::size_t at = spec.rfind('@');
    if (at != std::string_view::npos) {
      addr.mailbox_name.assign(spec.substr(0, at));
      addr.host_name.assign(spec.substr(at + 1));
    } else {
      addr.mailbox_name.assign(spec);
    }
    if (end >= text.size()) break;
  }
}

/// Splits a header block into unfolded fields and fills the well-known
/// members of `mail_header`. Field values are stored as received
/// (unfolded only); RFC 2047 encoded-word decoding is the
/// bkmail::detail::decode_encoded_words caller's job (mail_header.h).
template <class Allocator>
bool parse_mail_header_block(std::string_view block, const Allocator& alloc,
                             bkmail::mail_header<Allocator>& out) {
  using string_type = bkmail::detail::string_of<Allocator>;
  auto assign_field = [&](std::string_view name, std::string_view value) {
    // Trim trailing whitespace from the unfolded value.
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    if (ascii_iequals(name, "Subject")) {
      out.subject.assign(value);
    } else if (ascii_iequals(name, "Date")) {
      out.date.assign(value);
    } else if (ascii_iequals(name, "Message-ID")) {
      out.message_id.assign(value);
    } else if (ascii_iequals(name, "In-Reply-To")) {
      out.in_reply_to.assign(value);
    } else if (ascii_iequals(name, "From")) {
      parse_rfc5322_addresses<Allocator>(value, out.from);
    } else if (ascii_iequals(name, "To")) {
      parse_rfc5322_addresses<Allocator>(value, out.to);
    } else if (ascii_iequals(name, "Cc")) {
      parse_rfc5322_addresses<Allocator>(value, out.cc);
    } else if (ascii_iequals(name, "Bcc")) {
      parse_rfc5322_addresses<Allocator>(value, out.bcc);
    }
  };

  std::string_view current_name;
  string_type current_value(alloc);
  auto flush = [&] {
    if (!current_name.empty()) assign_field(current_name, current_value);
    current_name = {};
    current_value.clear();
  };

  while (!block.empty()) {
    // Take one line.
    const std::size_t eol = block.find('\n');
    std::string_view line = block.substr(0, eol);
    block = eol == std::string_view::npos ? std::string_view{}
                                          : block.substr(eol + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) {
      flush();
      continue;
    }
    if (line.front() == ' ' || line.front() == '\t') {
      // Continuation: unfold by appending.
      current_value.push_back(' ');
      const std::size_t first = line.find_first_not_of(" \t");
      if (first != std::string_view::npos) {
        current_value.append(line.substr(first));
      }
      continue;
    }
    flush();
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;  // Tolerate junk lines.
    current_name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    const std::size_t first = value.find_first_not_of(" \t");
    if (first != std::string_view::npos) value.remove_prefix(first);
    current_value.assign(value);
  }
  flush();
  return true;
}

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_FETCH_PARSE_H_
