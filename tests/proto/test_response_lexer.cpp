/**
 * @file tests/proto/test_response_lexer.cpp
 * @brief Unit tests for the two-mode (line/literal) IMAP response framer.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/detail/response_lexer.h>
#include <bnio/buffer/dynamic_byte_vector.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace det = bkmail::imap::detail;

/// Storage + buffer adapter + lexer bundle, mirroring the read pump's
/// setup; feed() appends committed bytes the way a read completion does.
struct lex_fixture {
  std::vector<std::byte> storage;
  bnio::dynamic_byte_vector_buffer<std::allocator<std::byte>> buffer{storage};
  det::response_lexer<> lexer;

  void feed(std::string_view bytes) {
    const bnio::mutable_buffer region = buffer.prepare(bytes.size());
    std::memcpy(region.data(), bytes.data(), bytes.size());
    buffer.commit(bytes.size());
  }
};

// Line mode: one CRLF-terminated line per response.
TEST(ResponseLexer, LineModeFraming) {
  lex_fixture f;
  f.feed("* OK ready\r\n* 3 EXISTS\r\n");

  const auto first = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ("* OK ready", *first);
  f.lexer.consume_response(f.buffer);

  const auto second = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ("* 3 EXISTS", *second);
  f.lexer.consume_response(f.buffer);

  EXPECT_EQ(0U, f.buffer.size());
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
}

// A partial line yields nullopt until its CRLF arrives.
TEST(ResponseLexer, PartialLineWaitsForMoreBytes) {
  lex_fixture f;
  f.feed("* OK rea");
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
  f.feed("dy\r");
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
  f.feed("\n");

  const auto response = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ("* OK ready", *response);
  f.lexer.consume_response(f.buffer);
}

// A CRLF split across two reads must still frame correctly.
TEST(ResponseLexer, CrlfSplitAcrossReads) {
  lex_fixture f;
  f.feed("a1 OK x\r");
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
  f.feed("\n* 1 EXISTS\r\n");

  const auto first = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ("a1 OK x", *first);
  f.lexer.consume_response(f.buffer);

  const auto second = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ("* 1 EXISTS", *second);
  f.lexer.consume_response(f.buffer);
}

// A {n} trailer at end of line switches to literal mode: the literal
// octets and the following line form ONE response.
TEST(ResponseLexer, LiteralTrailerSwitchesMode) {
  lex_fixture f;
  f.feed("* 1 FETCH (BODY[] {5}\r\nhello)\r\n");

  const auto response = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ("* 1 FETCH (BODY[] {5}\r\nhello)", *response);
  f.lexer.consume_response(f.buffer);
  EXPECT_EQ(0U, f.buffer.size());
}

// LITERAL+ ({n+}) and the obsolete LITERAL- (~{n}) spellings.
TEST(ResponseLexer, LiteralPlusAndMinusSpellings) {
  lex_fixture f;
  f.feed("* 1 FETCH (X {3+}\r\nabc)\r\n");
  const auto plus = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(plus.has_value());
  EXPECT_EQ("* 1 FETCH (X {3+}\r\nabc)", *plus);
  f.lexer.consume_response(f.buffer);

  f.feed("* 2 FETCH (X ~{4}\r\nwxyz)\r\n");
  const auto minus = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(minus.has_value());
  EXPECT_EQ("* 2 FETCH (X ~{4}\r\nwxyz)", *minus);
  f.lexer.consume_response(f.buffer);
}

// Literal octets are never scanned: embedded CRLF and fake response lines
// inside a literal must not split the response.
TEST(ResponseLexer, LiteralOctetsAreNeverScanned) {
  lex_fixture f;
  // The 6-octet literal "a\r\n* b" contains both a CRLF and a fake `*`
  // line start; a following real response must stay separate.
  f.feed("* 1 FETCH (X {6}\r\na\r\n* b)\r\n* 2 EXISTS\r\n");

  const auto first = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ("* 1 FETCH (X {6}\r\na\r\n* b)", *first);
  f.lexer.consume_response(f.buffer);

  const auto second = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ("* 2 EXISTS", *second);
  f.lexer.consume_response(f.buffer);
}

// Feeding one byte at a time must produce exactly the same framing.
TEST(ResponseLexer, ByteByByteFeeding) {
  lex_fixture f;
  constexpr std::string_view wire = "* 1 FETCH (X {4}\r\np\r\nq)\r\n";
  std::optional<std::string_view> response;
  for (const char ch : wire) {
    f.feed(std::string_view(&ch, 1));
    response = f.lexer.next_complete_response(f.buffer);
  }
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ("* 1 FETCH (X {4}\r\np\r\nq)", *response);
  f.lexer.consume_response(f.buffer);
}

// A literal split across several reads completes only when every octet
// (and the terminating line) has arrived.
TEST(ResponseLexer, LiteralSplitAcrossReads) {
  lex_fixture f;
  f.feed("* 1 FETCH (X {10}\r\nhe");
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
  f.feed("llo ");
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
  // Exactly n octets are still not enough: the terminating line is missing.
  f.feed("world");
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
  f.feed(")\r\n");

  const auto response = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ("* 1 FETCH (X {10}\r\nhello world)", *response);
  f.lexer.consume_response(f.buffer);
}

// consume_response drops exactly the response bytes; the cursor restarts
// at the next response.
TEST(ResponseLexer, ConsumeSemantics) {
  lex_fixture f;
  f.feed("* OK one\r\n* OK two\r\n* OK thr");
  const auto first = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ("* OK one", *first);

  f.lexer.consume_response(f.buffer);
  EXPECT_EQ(std::string_view("* OK two\r\n* OK thr").size(), f.buffer.size());

  const auto second = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ("* OK two", *second);
  f.lexer.consume_response(f.buffer);

  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());
}

// Trailer edge cases: a marker that is not a token of its own, not at end
// of line, or with overflowing digits is NOT a literal trailer.
TEST(ResponseLexer, FalseTrailersStayPlainLines) {
  lex_fixture f;
  // "{5}" not preceded by SP or '~': not a trailer token.
  f.feed("* X a{5}\r\n");
  const auto glued = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(glued.has_value());
  EXPECT_EQ("* X a{5}", *glued);
  f.lexer.consume_response(f.buffer);

  // "{5}" not at end of line.
  f.feed("* X {5} tail\r\n");
  const auto mid = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(mid.has_value());
  EXPECT_EQ("* X {5} tail", *mid);
  f.lexer.consume_response(f.buffer);

  // Digit run overflowing 64 bits: implausible count, plain text.
  f.feed("* X {99999999999999999999999999}\r\n");
  const auto huge = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(huge.has_value());
  EXPECT_EQ("* X {99999999999999999999999999}", *huge);
  f.lexer.consume_response(f.buffer);
}

// A zero-length literal switches modes without consuming octets. (The
// trailer must be a token of its own, preceded by SP.)
TEST(ResponseLexer, EmptyLiteral) {
  lex_fixture f;
  f.feed("* X (X {0}\r\n)\r\n");
  const auto response = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ("* X (X {0}\r\n)", *response);
  f.lexer.consume_response(f.buffer);
}

// reset() abandons all cursor state; extraction restarts from the buffer
// front (teardown/resynchronization path).
TEST(ResponseLexer, ResetClearsCursorState) {
  lex_fixture f;
  f.feed("* OK par");
  EXPECT_FALSE(f.lexer.next_complete_response(f.buffer).has_value());

  f.lexer.reset();
  f.feed("tial\r\n");
  const auto response = f.lexer.next_complete_response(f.buffer);
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ("* OK partial", *response);
  f.lexer.consume_response(f.buffer);
}

}  // namespace
