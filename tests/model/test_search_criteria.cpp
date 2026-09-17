/**
 * @file tests/model/test_search_criteria.cpp
 * @brief Unit tests for bkmail::imap::search_criteria.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/search_criteria.h>
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

namespace {

using bkmail::imap::search_criteria;

// -- Normal cases ------------------------------------------------------------

TEST(SearchCriteria, AcceptsTypicalKeys) {
  EXPECT_TRUE(search_criteria<>::is_valid("ALL"));
  EXPECT_TRUE(search_criteria<>::is_valid("UNSEEN FROM \"bob\""));
  EXPECT_TRUE(search_criteria<>::is_valid("SINCE 1-Sep-2026 NOT DELETED"));
  EXPECT_TRUE(search_criteria<>::is_valid("TEXT hello"));
}

TEST(SearchCriteria, AcceptsBalancedParentheses) {
  EXPECT_TRUE(search_criteria<>::is_valid("(OR FROM \"a\" TO \"b\")"));
  EXPECT_TRUE(search_criteria<>::is_valid("((ALL))"));
  EXPECT_TRUE(search_criteria<>::is_valid("NOT (SEEN)"));
}

TEST(SearchCriteria, AcceptsQuotedStringsWithEscapes) {
  EXPECT_TRUE(search_criteria<>::is_valid("FROM \"alice \\\"the boss\\\"\""));
  EXPECT_TRUE(search_criteria<>::is_valid("SUBJECT \"C:\\\\path\""));
  // Parentheses inside quotes do not count towards depth.
  EXPECT_TRUE(search_criteria<>::is_valid("SUBJECT \"(unbalanced\""));
}

TEST(SearchCriteria, ConstructorStoresValidatedText) {
  const search_criteria<> criteria("UNSEEN FROM \"bob\"");
  EXPECT_EQ("UNSEEN FROM \"bob\"", criteria.str());
  EXPECT_FALSE(criteria.empty());
}

TEST(SearchCriteria, ParseReturnsEngagedOptionalOnValidInput) {
  const auto parsed = search_criteria<>::parse("ALL");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ("ALL", parsed->str());
}

// -- Boundary cases ----------------------------------------------------------

TEST(SearchCriteria, DefaultConstructedIsEmpty) {
  const search_criteria<> criteria;
  EXPECT_TRUE(criteria.empty());
  EXPECT_EQ("", criteria.str());
}

TEST(SearchCriteria, SingleCharacterKeyIsValid) {
  EXPECT_TRUE(search_criteria<>::is_valid("A"));
}

TEST(SearchCriteria, EmptyQuotedStringIsBalanced) {
  EXPECT_TRUE(search_criteria<>::is_valid("SUBJECT \"\""));
}

// -- Abnormal cases ----------------------------------------------------------

TEST(SearchCriteria, RejectsEmptyText) {
  EXPECT_FALSE(search_criteria<>::is_valid(""));
  EXPECT_EQ(std::nullopt, search_criteria<>::parse(""));
  EXPECT_THROW((void)search_criteria<>(""), std::invalid_argument);
}

TEST(SearchCriteria, RejectsCarriageReturnAndLineFeed) {
  EXPECT_FALSE(search_criteria<>::is_valid("ALL\r"));
  EXPECT_FALSE(search_criteria<>::is_valid("ALL\n"));
  EXPECT_FALSE(search_criteria<>::is_valid("UNSEEN\r\nFROM \"bob\""));
  // CR/LF is rejected even inside a quoted string.
  EXPECT_FALSE(search_criteria<>::is_valid("SUBJECT \"a\nb\""));
}

TEST(SearchCriteria, RejectsUnbalancedParentheses) {
  EXPECT_FALSE(search_criteria<>::is_valid("("));
  EXPECT_FALSE(search_criteria<>::is_valid(")"));
  EXPECT_FALSE(search_criteria<>::is_valid("(ALL"));
  EXPECT_FALSE(search_criteria<>::is_valid("ALL)"));
  EXPECT_FALSE(search_criteria<>::is_valid("((ALL)"));
  EXPECT_FALSE(search_criteria<>::is_valid("(ALL))"));
}

TEST(SearchCriteria, RejectsUnbalancedQuotes) {
  EXPECT_FALSE(search_criteria<>::is_valid("SUBJECT \"hello"));
  EXPECT_FALSE(search_criteria<>::is_valid("SUBJECT hello\""));
  EXPECT_FALSE(search_criteria<>::is_valid("\""));
}

TEST(SearchCriteria, EscapedQuoteDoesNotCloseTheString) {
  // The closing quote is escaped, so the string never terminates.
  EXPECT_FALSE(search_criteria<>::is_valid("SUBJECT \"a\\\"b"));
  // Trailing backslash escapes the would-be closing quote.
  EXPECT_FALSE(search_criteria<>::is_valid("SUBJECT \"a\\"));
}

TEST(SearchCriteria, ConstructorThrowsOnInvalidInput) {
  EXPECT_THROW((void)search_criteria<>("ALL\r\nBAD"), std::invalid_argument);
  EXPECT_THROW((void)search_criteria<>("(ALL"), std::invalid_argument);
  EXPECT_THROW((void)search_criteria<>("\""), std::invalid_argument);
}

TEST(SearchCriteria, ParseReturnsNulloptOnInvalidInput) {
  EXPECT_EQ(std::nullopt, search_criteria<>::parse("ALL\n"));
  EXPECT_EQ(std::nullopt, search_criteria<>::parse(")"));
  EXPECT_EQ(std::nullopt, search_criteria<>::parse("SUBJECT \"x"));
}

}  // namespace
