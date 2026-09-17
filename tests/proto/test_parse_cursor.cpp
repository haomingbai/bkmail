/**
 * @file tests/proto/test_parse_cursor.cpp
 * @brief Unit tests for the non-owning deep-parse cursor over one complete
 *        IMAP response.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/detail/parse_cursor.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace det = bkmail::imap::detail;

using cursor = det::parse_cursor<>;

// Basic positioning, peeking, and single-character consumption.
TEST(ParseCursor, PositioningAndConsumption) {
  cursor c("ab cd");
  EXPECT_FALSE(c.at_end());
  EXPECT_EQ('a', c.peek());
  EXPECT_EQ(0U, c.position());
  EXPECT_EQ("ab cd", c.remaining());

  EXPECT_TRUE(c.try_consume('a'));
  EXPECT_FALSE(c.try_consume('a'));
  EXPECT_EQ('b', c.peek());

  c.skip_spaces();  // No spaces here: nothing moves.
  EXPECT_EQ(1U, c.position());

  EXPECT_TRUE(c.try_consume('b'));
  EXPECT_TRUE(c.try_consume(' '));
  EXPECT_EQ("cd", c.remaining());

  EXPECT_TRUE(c.try_consume_ci("CD"));  // Case-insensitive word consume.
  EXPECT_TRUE(c.at_end());
  EXPECT_EQ('\0', c.peek());
  EXPECT_FALSE(c.try_consume('x'));
}

TEST(ParseCursor, TryConsumeCiPrefixRules) {
  cursor c("fetchx");
  EXPECT_FALSE(c.try_consume_ci("fetched"));
  EXPECT_EQ(0U, c.position());
  EXPECT_TRUE(c.try_consume_ci("FETCH"));
  EXPECT_EQ(5U, c.position());
  EXPECT_EQ("x", c.remaining());
}

// Atom reading stops at atom-specials and may be empty.
TEST(ParseCursor, ReadAtom) {
  cursor c("abc (def");
  EXPECT_EQ("abc", c.read_atom());
  EXPECT_EQ(3U, c.position());

  cursor at_special("(x");
  EXPECT_EQ("", at_special.read_atom());
  EXPECT_EQ(0U, at_special.position());

  cursor at_end("");
  EXPECT_EQ("", at_end.read_atom());
}

// Unsigned decimal numbers; empty and overflow both yield nullopt.
TEST(ParseCursor, ReadNumber) {
  cursor c("123 rest");
  const auto value = c.read_number();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(123U, *value);
  EXPECT_EQ(3U, c.position());

  cursor no_digits("abc");
  EXPECT_FALSE(no_digits.read_number().has_value());
  EXPECT_EQ(0U, no_digits.position());  // No digit consumed.

  cursor overflow("99999999999999999999999999");
  EXPECT_FALSE(overflow.read_number().has_value());

  cursor zero("0");
  const auto z = zero.read_number();
  ASSERT_TRUE(z.has_value());
  EXPECT_EQ(0U, *z);
}

// Quoted strings: only \" and \\ escapes are legal.
TEST(ParseCursor, ReadQuoted) {
  cursor plain("\"hello world\"");
  const auto p = plain.read_quoted();
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ("hello world", *p);
  EXPECT_TRUE(plain.at_end());

  // Escaped DQUOTE and backslash materialize through the scratch buffer.
  cursor escaped("\"a\\\"b\\\\c\"");
  const auto e = escaped.read_quoted();
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ("a\"b\\c", *e);

  cursor empty("\"\"");
  const auto em = empty.read_quoted();
  ASSERT_TRUE(em.has_value());
  EXPECT_EQ("", *em);
}

TEST(ParseCursor, ReadQuotedMalformed) {
  cursor no_quote("abc");
  EXPECT_FALSE(no_quote.read_quoted().has_value());

  cursor unterminated("\"abc");
  EXPECT_FALSE(unterminated.read_quoted().has_value());

  // \n is not a legal quoted escape (only \" and \\ are).
  cursor bad_escape("\"a\\nb\"");
  EXPECT_FALSE(bad_escape.read_quoted().has_value());

  // A raw CR inside a quoted string is malformed.
  cursor embedded_cr("\"a\rb\"");
  EXPECT_FALSE(embedded_cr.read_quoted().has_value());
}

// Literals: {n} / {n+} / ~{n} spellings, octets returned verbatim.
TEST(ParseCursor, ReadLiteral) {
  cursor basic("{3}\r\nabc");
  const auto b = basic.read_literal();
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ("abc", *b);
  EXPECT_TRUE(basic.at_end());

  cursor plus("{2+}\r\nxy");
  const auto p = plus.read_literal();
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ("xy", *p);

  // Obsolete LITERAL- spelling.
  cursor minus("~{4}\r\nwxyz");
  const auto m = minus.read_literal();
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ("wxyz", *m);

  // Literal octets may contain CRLF and quotes; they are never interpreted.
  cursor raw("{5}\r\na\r\n\"b tail");
  const auto r = raw.read_literal();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::string_view("a\r\n\"b", 5), *r);
  EXPECT_EQ(" tail", raw.remaining());
}

TEST(ParseCursor, ReadLiteralMalformed) {
  cursor no_brace("abc");
  EXPECT_FALSE(no_brace.read_literal().has_value());

  cursor no_digits("{}\r\n");
  EXPECT_FALSE(no_digits.read_literal().has_value());

  cursor missing_brace("{3\r\nabc");
  EXPECT_FALSE(missing_brace.read_literal().has_value());

  // '}' must be followed by CRLF immediately.
  cursor missing_crlf("{3} abc");
  EXPECT_FALSE(missing_crlf.read_literal().has_value());

  // Declared more octets than the response holds (defensive; the lexer
  // normally guarantees presence).
  cursor truncated("{9}\r\nabc");
  EXPECT_FALSE(truncated.read_literal().has_value());
}

// read_string accepts quoted or literal, nothing else.
TEST(ParseCursor, ReadString) {
  cursor q("\"text\"");
  const auto qv = q.read_string();
  ASSERT_TRUE(qv.has_value());
  EXPECT_EQ("text", *qv);

  cursor l("{4}\r\nabcd");
  const auto lv = l.read_string();
  ASSERT_TRUE(lv.has_value());
  EXPECT_EQ("abcd", *lv);

  cursor atom("ATOM");
  EXPECT_FALSE(atom.read_string().has_value());
}

// read_astring accepts atom, quoted, or literal.
TEST(ParseCursor, ReadAstring) {
  cursor atom("INBOX rest");
  const auto a = atom.read_astring();
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ("INBOX", *a);

  cursor quoted("\"a b\"");
  const auto q = quoted.read_astring();
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ("a b", *q);

  cursor literal("{3}\r\nxyz");
  const auto l = literal.read_astring();
  ASSERT_TRUE(l.has_value());
  EXPECT_EQ("xyz", *l);

  cursor empty("(not an astring");
  EXPECT_FALSE(empty.read_astring().has_value());
}

// read_nstring: NIL <-> disengaged inner optional; malformed -> disengaged
// outer optional.
TEST(ParseCursor, ReadNstring) {
  cursor nil("NIL");
  const auto n = nil.read_nstring();
  ASSERT_TRUE(n.has_value());
  EXPECT_FALSE(n->has_value());
  EXPECT_TRUE(nil.at_end());

  // NIL followed by a non-atom character still reads as NIL.
  cursor nil_paren("NIL)");
  const auto np = nil_paren.read_nstring();
  ASSERT_TRUE(np.has_value());
  EXPECT_FALSE(np->has_value());
  EXPECT_EQ(")", nil_paren.remaining());

  // An atom starting with NIL is not NIL: the outer optional disengages
  // because "NILA" is not a string either.
  cursor nil_atom("NILA");
  EXPECT_FALSE(nil_atom.read_nstring().has_value());

  cursor str("\"subject\"");
  const auto s = str.read_nstring();
  ASSERT_TRUE(s.has_value());
  ASSERT_TRUE(s->has_value());
  EXPECT_EQ("subject", **s);

  cursor lit("{2}\r\nhi");
  const auto l = lit.read_nstring();
  ASSERT_TRUE(l.has_value());
  ASSERT_TRUE(l->has_value());
  EXPECT_EQ("hi", **l);
}

// read_until_any stops at (and before) the first stop character.
TEST(ParseCursor, ReadUntilAny) {
  cursor c("abc def)");
  EXPECT_EQ("abc", c.read_until_any(" "));
  EXPECT_EQ(3U, c.position());

  EXPECT_TRUE(c.try_consume(' '));
  EXPECT_EQ("def", c.read_until_any(")"));
  EXPECT_EQ(")", c.remaining());

  // A stop character at the cursor yields an empty view without moving.
  cursor at_stop("(x");
  EXPECT_EQ("", at_stop.read_until_any("("));
  EXPECT_EQ(0U, at_stop.position());
}

}  // namespace
