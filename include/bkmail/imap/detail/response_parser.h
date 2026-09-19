/**
 * @file include/bkmail/imap/detail/response_parser.h
 * @brief IMAP server response classifier and shallow parser.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#pragma once
#ifndef BKMAIL_IMAP_DETAIL_RESPONSE_PARSER_H_
#define BKMAIL_IMAP_DETAIL_RESPONSE_PARSER_H_

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/imap/detail/astring.h>
#include <bkmail/imap/detail/parse_cursor.h>
#include <bkmail/imap/flags.h>
#include <bkmail/imap/response.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace bkmail::imap::detail {

/**
 * Classifies and shallow-parses one complete response into the wire types of
 * `imap/response.h`: tagged reply, untagged response, or continuation
 * request. Deep structured parsing (ENVELOPE lists, BODYSTRUCTURE trees,
 * FETCH attribute lists) is left to the consuming command, which walks
 * `untagged_response::payload` with `parse_cursor` (detail/parse_cursor.h).
 *
 * Robustness contract: unknown resp-text-codes degrade to `unknown_code`
 * (never a parse failure); unknown untagged keywords degrade to
 * `untagged_kind::unknown` with the raw remainder in `payload`. Only bytes
 * that cannot be a well-formed response at all yield `std::nullopt`.
 */
template <class Allocator = std::allocator<std::byte>>
class response_parser {
 public:
  /// Allocator type threaded into every owning string of the result.
  using allocator_type = Allocator;
  /// Allocator-aware string type.
  using string_type = bkmail::detail::string_of<Allocator>;
  /// The parsed response variant.
  using response_type = server_response<Allocator>;

  /// Creates a parser whose results allocate through `alloc`.
  explicit response_parser(const Allocator& alloc = Allocator{}) noexcept
      : alloc_(alloc) {}

  /**
   * Parses one complete response as extracted by `response_lexer` (final
   * CRLF already stripped). Returns `std::nullopt` when the bytes are not a
   * well-formed response (a protocol violation; the caller fails the
   * connection).
   */
  [[nodiscard]] std::optional<response_type> parse(
      std::string_view response) const {
    if (response.empty()) {
      return std::nullopt;
    }
    switch (response.front()) {
      case '+':
        return parse_continuation(response);
      case '*':
        return parse_untagged(response);
      default:
        return parse_tagged(response);
    }
  }

 private:
  static std::optional<response_status> classify_status(
      std::string_view word) noexcept {
    if (ascii_iequals(word, "OK")) {
      return response_status::ok;
    }
    if (ascii_iequals(word, "NO")) {
      return response_status::no;
    }
    if (ascii_iequals(word, "BAD")) {
      return response_status::bad;
    }
    if (ascii_iequals(word, "PREAUTH")) {
      return response_status::preauth;
    }
    if (ascii_iequals(word, "BYE")) {
      return response_status::bye;
    }
    return std::nullopt;
  }

  static untagged_kind kind_for(response_status status) noexcept {
    switch (status) {
      case response_status::ok:
        return untagged_kind::ok;
      case response_status::no:
        return untagged_kind::no;
      case response_status::bad:
        return untagged_kind::bad;
      case response_status::preauth:
        return untagged_kind::preauth;
      case response_status::bye:
        return untagged_kind::bye;
    }
    return untagged_kind::unknown;  // Unreachable.
  }

  static void map_flag(std::string_view flag, flag_set& flags) {
    if (ascii_iequals(flag, "\\Seen")) {
      flags.set(message_flag::seen);
    } else if (ascii_iequals(flag, "\\Answered")) {
      flags.set(message_flag::answered);
    } else if (ascii_iequals(flag, "\\Flagged")) {
      flags.set(message_flag::flagged);
    } else if (ascii_iequals(flag, "\\Deleted")) {
      flags.set(message_flag::deleted);
    } else if (ascii_iequals(flag, "\\Draft")) {
      flags.set(message_flag::draft);
    } else if (ascii_iequals(flag, "\\Recent")) {
      flags.set(message_flag::recent);
    }
    // Unknown flags (including bare keywords) are ignored by design.
  }

  response_code<Allocator> make_unknown(std::string_view atom,
                                        std::string_view arguments) const {
    if (!arguments.empty() && arguments.front() == ' ') {
      arguments.remove_prefix(1);
    }
    unknown_code<Allocator> code(alloc_);
    code.atom.assign(atom.data(), atom.size());
    code.arguments.assign(arguments.data(), arguments.size());
    return code;
  }

  /// Parses the inside of a `[resp-text-code]` bracket. Always succeeds:
  /// anything unrecognized degrades to `unknown_code`.
  response_code<Allocator> parse_resp_code(std::string_view inner) const {
    parse_cursor<Allocator> c(inner, alloc_);
    const std::string_view atom = c.read_atom();

    if (ascii_iequals(atom, "ALERT")) {
      return alert_code{};
    }
    if (ascii_iequals(atom, "PARSE")) {
      return parse_code{};
    }
    if (ascii_iequals(atom, "READ-ONLY")) {
      return read_only_code{};
    }
    if (ascii_iequals(atom, "READ-WRITE")) {
      return read_write_code{};
    }
    if (ascii_iequals(atom, "TRYCREATE")) {
      return trycreate_code{};
    }

    if (ascii_iequals(atom, "UIDNEXT") || ascii_iequals(atom, "UIDVALIDITY") ||
        ascii_iequals(atom, "UNSEEN")) {
      if (c.try_consume(' ')) {
        if (const auto n = c.read_number()) {
          const auto v = static_cast<std::uint32_t>(*n & 0xffffffffu);
          if (ascii_iequals(atom, "UIDNEXT")) {
            return uidnext_code{v};
          }
          if (ascii_iequals(atom, "UIDVALIDITY")) {
            return uidvalidity_code{v};
          }
          return unseen_code{v};
        }
      }
      return make_unknown(atom, c.remaining());
    }

    if (ascii_iequals(atom, "CAPABILITY")) {
      capability_code<Allocator> code(alloc_);
      while (c.try_consume(' ')) {
        const std::string_view cap = c.read_atom();
        if (cap.empty()) {
          break;
        }
        code.capabilities.emplace_back(cap.data(), cap.size());
      }
      return code;
    }

    if (ascii_iequals(atom, "BADCHARSET")) {
      badcharset_code<Allocator> code(alloc_);
      if (!c.try_consume(' ')) {
        return code;  // BADCHARSET without a charset list is legal.
      }
      if (!c.try_consume('(')) {
        return make_unknown(atom, c.remaining());
      }
      for (;;) {
        c.skip_spaces();
        if (c.try_consume(')')) {
          break;
        }
        const auto cs = c.read_astring();
        if (!cs) {
          return make_unknown(atom, c.remaining());
        }
        code.charsets.emplace_back(cs->data(), cs->size());
      }
      return code;
    }

    if (ascii_iequals(atom, "PERMANENTFLAGS")) {
      permanentflags_code code{};
      if (!c.try_consume(' ') || !c.try_consume('(')) {
        return make_unknown(atom, c.remaining());
      }
      for (;;) {
        c.skip_spaces();
        if (c.try_consume(')')) {
          break;
        }
        const std::string_view flag = c.read_until_any(" )");
        if (flag.empty()) {
          return make_unknown(atom, c.remaining());
        }
        map_flag(flag, code.flags);
      }
      return code;
    }

    return make_unknown(atom, c.remaining());
  }

  /// Parses resp-text: `["[" resp-text-code "]" SP] TEXT`. Returns false only
  /// on an unterminated `[` code bracket.
  bool parse_resp_text(parse_cursor<Allocator>& c,
                       std::optional<response_code<Allocator>>& code,
                       string_type& text) const {
    code.reset();
    text.clear();
    if (!c.try_consume(' ')) {
      return true;  // No resp-text (tolerated; the grammar mandates SP TEXT).
    }
    if (c.peek() == '[') {
      c.try_consume('[');
      const std::string_view inner = c.read_until_any("]");
      if (!c.try_consume(']')) {
        return false;  // Unterminated resp-text-code.
      }
      code = parse_resp_code(inner);
      if (c.try_consume(' ')) {
        const std::string_view rest = c.remaining();
        text.assign(rest.data(), rest.size());
      }
    } else {
      const std::string_view rest = c.remaining();
      text.assign(rest.data(), rest.size());
    }
    return true;
  }

  std::optional<response_type> parse_continuation(
      std::string_view response) const {
    continuation_request<Allocator> out(alloc_);
    std::string_view rest = response.substr(1);
    if (!rest.empty() && rest.front() == ' ') {
      rest.remove_prefix(1);
    }
    out.text.assign(rest.data(), rest.size());
    return response_type{std::move(out)};
  }

  std::optional<response_type> parse_tagged(std::string_view response) const {
    parse_cursor<Allocator> c(response, alloc_);
    const std::string_view tag = c.read_atom();
    if (tag.empty() || !c.try_consume(' ')) {
      return std::nullopt;
    }
    const auto status = classify_status(c.read_atom());
    if (!status || *status == response_status::preauth ||
        *status == response_status::bye) {
      return std::nullopt;  // Tagged replies are OK / NO / BAD only.
    }
    tagged_response<Allocator> out(alloc_);
    out.tag.assign(tag.data(), tag.size());
    out.status = *status;
    if (!parse_resp_text(c, out.code, out.text)) {
      return std::nullopt;
    }
    return response_type{std::move(out)};
  }

  std::optional<response_type> parse_untagged(std::string_view response) const {
    parse_cursor<Allocator> c(response, alloc_);
    c.try_consume('*');  // Guaranteed by the caller's classification.
    if (!c.try_consume(' ')) {
      return std::nullopt;
    }
    untagged_response<Allocator> out(alloc_);
    const std::size_t data_start = c.position();

    if (is_digit(c.peek())) {
      const auto number = c.read_number();
      if (!number || !c.try_consume(' ')) {
        return std::nullopt;
      }
      out.number = *number;
      const std::string_view word = c.read_atom();
      if (ascii_iequals(word, "EXISTS")) {
        out.kind = untagged_kind::exists;
      } else if (ascii_iequals(word, "RECENT")) {
        out.kind = untagged_kind::recent;
      } else if (ascii_iequals(word, "EXPUNGE")) {
        out.kind = untagged_kind::expunge;
      } else if (ascii_iequals(word, "FETCH")) {
        out.kind = untagged_kind::fetch;
        if (c.try_consume(' ')) {
          out.payload = c.remaining();
        }
      } else {
        // Numbered extension data (e.g. VANISHED): keep the raw remainder.
        out.kind = untagged_kind::unknown;
        out.payload = response.substr(data_start);
      }
      return response_type{std::move(out)};
    }

    const std::string_view word = c.read_atom();
    if (word.empty()) {
      return std::nullopt;
    }
    if (const auto status = classify_status(word)) {
      out.kind = kind_for(*status);
      out.status = *status;
      if (!parse_resp_text(c, out.code, out.text)) {
        return std::nullopt;
      }
      return response_type{std::move(out)};
    }
    if (ascii_iequals(word, "CAPABILITY")) {
      out.kind = untagged_kind::capability;
    } else if (ascii_iequals(word, "FLAGS")) {
      out.kind = untagged_kind::flags;
    } else if (ascii_iequals(word, "LIST")) {
      out.kind = untagged_kind::list;
    } else if (ascii_iequals(word, "LSUB")) {
      out.kind = untagged_kind::lsub;
    } else if (ascii_iequals(word, "STATUS")) {
      out.kind = untagged_kind::status;
    } else if (ascii_iequals(word, "SEARCH")) {
      out.kind = untagged_kind::search;
    } else {
      // Unknown/extension keyword: keep keyword + remainder, never fail.
      out.kind = untagged_kind::unknown;
      out.payload = response.substr(data_start);
      return response_type{std::move(out)};
    }
    if (c.try_consume(' ')) {
      out.payload = c.remaining();
    }
    return response_type{std::move(out)};
  }

  Allocator alloc_;
};

}  // namespace bkmail::imap::detail

#endif  // BKMAIL_IMAP_DETAIL_RESPONSE_PARSER_H_
