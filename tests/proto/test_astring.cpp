/**
 * @file tests/proto/test_astring.cpp
 * @brief Unit tests for the IMAP astring/quoted/literal character classes
 *        and rendering helpers.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/detail/astring.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

namespace det = bkmail::imap::detail;

// RFC 3501 §9 character classes.
TEST(AstringChars, CharAndCtl) {
  EXPECT_TRUE(det::is_char('\x01'));
  EXPECT_TRUE(det::is_char('a'));
  EXPECT_TRUE(det::is_char('\x7f'));
  EXPECT_FALSE(det::is_char('\0'));
  EXPECT_FALSE(det::is_char(static_cast<char>(0x80)));

  EXPECT_TRUE(det::is_ctl('\x00'));
  EXPECT_TRUE(det::is_ctl('\x1f'));
  EXPECT_TRUE(det::is_ctl('\x7f'));
  EXPECT_FALSE(det::is_ctl(' '));
  EXPECT_FALSE(det::is_ctl('a'));
}

TEST(AstringChars, AtomAndAstringAndTagChars) {
  // Plain atom characters.
  EXPECT_TRUE(det::is_atom_char('a'));
  EXPECT_TRUE(det::is_atom_char('0'));
  EXPECT_TRUE(det::is_atom_char('-'));
  EXPECT_TRUE(det::is_atom_char(']') == false);  // ']' is a resp-special.

  // Every atom-special is excluded from ATOM-CHAR.
  EXPECT_FALSE(det::is_atom_char('('));
  EXPECT_FALSE(det::is_atom_char(')'));
  EXPECT_FALSE(det::is_atom_char('{'));
  EXPECT_FALSE(det::is_atom_char(' '));
  EXPECT_FALSE(det::is_atom_char('%'));
  EXPECT_FALSE(det::is_atom_char('*'));
  EXPECT_FALSE(det::is_atom_char('"'));
  EXPECT_FALSE(det::is_atom_char('\\'));
  EXPECT_FALSE(det::is_atom_char(']'));
  EXPECT_FALSE(det::is_atom_char('\t'));

  // ASTRING-CHAR adds resp-specials ("]") back.
  EXPECT_TRUE(det::is_astring_char('a'));
  EXPECT_TRUE(det::is_astring_char(']'));
  EXPECT_FALSE(det::is_astring_char('('));
  EXPECT_FALSE(det::is_astring_char(' '));

  // Tag characters are ASTRING-CHAR minus '+'.
  EXPECT_TRUE(det::is_tag_char('a'));
  EXPECT_TRUE(det::is_tag_char(']'));
  EXPECT_FALSE(det::is_tag_char('+'));
  EXPECT_FALSE(det::is_tag_char(' '));
}

TEST(AstringChars, TextCharAndDigit) {
  EXPECT_TRUE(det::is_text_char('a'));
  EXPECT_TRUE(det::is_text_char(' '));
  EXPECT_FALSE(det::is_text_char('\r'));
  EXPECT_FALSE(det::is_text_char('\n'));
  EXPECT_FALSE(det::is_text_char(static_cast<char>(0x80)));

  EXPECT_TRUE(det::is_digit('0'));
  EXPECT_TRUE(det::is_digit('9'));
  EXPECT_FALSE(det::is_digit('a'));
}

TEST(AstringChars, AsciiCaseTools) {
  EXPECT_EQ('A', det::ascii_to_upper('a'));
  EXPECT_EQ('Z', det::ascii_to_upper('z'));
  EXPECT_EQ('A', det::ascii_to_upper('A'));
  EXPECT_EQ('0', det::ascii_to_upper('0'));
  EXPECT_EQ('[', det::ascii_to_upper('['));

  EXPECT_TRUE(det::ascii_iequals("Fetch", "FETCH"));
  EXPECT_TRUE(det::ascii_iequals("imap4REV1", "IMAP4rev1"));
  EXPECT_FALSE(det::ascii_iequals("abc", "abd"));
  EXPECT_FALSE(det::ascii_iequals("abc", "abcd"));
  EXPECT_TRUE(det::ascii_iequals("", ""));
}

// classify_astring: atom / quoted / literal three-way split.
TEST(AstringClassify, ThreeWaySplit) {
  EXPECT_EQ(det::astring_class::atom, det::classify_astring("INBOX"));
  EXPECT_EQ(det::astring_class::atom,
            det::classify_astring("a]b"));  // ']' is an ASTRING-CHAR.

  // The empty string cannot be a bare atom: it renders as "".
  EXPECT_EQ(det::astring_class::quoted, det::classify_astring(""));
  EXPECT_EQ(det::astring_class::quoted, det::classify_astring("has space"));
  EXPECT_EQ(det::astring_class::quoted, det::classify_astring("wild%card"));
  EXPECT_EQ(det::astring_class::quoted, det::classify_astring("quote\"d"));

  EXPECT_EQ(det::astring_class::literal, det::classify_astring("a\r\nb"));
  EXPECT_EQ(det::astring_class::literal,
            det::classify_astring(std::string_view("x\x80"
                                                   "y",
                                                   3)));
}

// render_astring / render_quoted escaping.
TEST(AstringRender, AtomVerbatimAndQuotedEscapes) {
  std::string out;
  det::render_astring("INBOX", out);
  EXPECT_EQ("INBOX", out);

  out.clear();
  det::render_astring("a b", out);
  EXPECT_EQ("\"a b\"", out);

  // Only `\"` and `\\` are escaped (RFC 3501 §9 quoted-specials).
  out.clear();
  det::render_quoted("q\"u\\o", out);
  EXPECT_EQ("\"q\\\"u\\\\o\"", out);

  // Wildcards are not escaped, only quoted.
  out.clear();
  det::render_astring("a%b", out);
  EXPECT_EQ("\"a%b\"", out);
}

// render_literal: both framing flavours.
TEST(AstringRender, LiteralFraming) {
  std::string out;
  det::render_literal("hello", out);
  EXPECT_EQ("{5}\r\nhello", out);

  out.clear();
  det::render_literal("ab", out, det::literal_mode::non_synchronizing);
  EXPECT_EQ("{2+}\r\nab", out);

  // Literal octets are emitted verbatim, CRLF included.
  out.clear();
  det::render_literal("x\r\ny", out);
  EXPECT_EQ("{4}\r\nx\r\ny", out);

  out.clear();
  det::render_literal("", out);
  EXPECT_EQ("{0}\r\n", out);
}

}  // namespace
