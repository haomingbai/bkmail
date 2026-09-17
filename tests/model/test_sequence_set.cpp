/**
 * @file tests/model/test_sequence_set.cpp
 * @brief Unit tests for bkmail::imap::sequence_set.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/sequence_set.h>
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using bkmail::imap::sequence_set;

// -- Normal cases: valid grammar accepted ------------------------------------

TEST(SequenceSet, AcceptsSingleNumber) {
  EXPECT_TRUE(sequence_set::is_valid("1"));
  EXPECT_TRUE(sequence_set::is_valid("42"));
  EXPECT_TRUE(sequence_set::is_valid("4294967295"));  // 2^32 - 1
}

TEST(SequenceSet, AcceptsStar) { EXPECT_TRUE(sequence_set::is_valid("*")); }

TEST(SequenceSet, AcceptsRanges) {
  EXPECT_TRUE(sequence_set::is_valid("2:4"));
  EXPECT_TRUE(sequence_set::is_valid("7:*"));
  EXPECT_TRUE(sequence_set::is_valid("*:*"));
  EXPECT_TRUE(sequence_set::is_valid("1:4294967295"));
}

TEST(SequenceSet, AcceptsCommaCombinations) {
  EXPECT_TRUE(sequence_set::is_valid("2:4,7:*"));
  EXPECT_TRUE(sequence_set::is_valid("1,2,3"));
  EXPECT_TRUE(sequence_set::is_valid("*,1:5,9"));
}

TEST(SequenceSet, ConstructorStoresValidatedText) {
  const sequence_set set("2:4,7:*");
  EXPECT_EQ("2:4,7:*", set.str());
  EXPECT_FALSE(set.empty());
}

TEST(SequenceSet, ParseReturnsEngagedOptionalOnValidInput) {
  const std::optional<sequence_set> parsed = sequence_set::parse("1:100,*");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ("1:100,*", parsed->str());
}

TEST(SequenceSet, EqualityComparesStoredText) {
  EXPECT_EQ(sequence_set("1:5"), sequence_set("1:5"));
  EXPECT_NE(sequence_set("1:5"), sequence_set("1:6"));
}

// -- Boundary cases ----------------------------------------------------------

TEST(SequenceSet, AcceptsUint32MaxButNotBeyond) {
  EXPECT_TRUE(sequence_set::is_valid("4294967295"));
  EXPECT_FALSE(sequence_set::is_valid("4294967296"));
  EXPECT_FALSE(sequence_set::is_valid("99999999999999999999"));
}

TEST(SequenceSet, DefaultConstructedIsEmpty) {
  const sequence_set set;
  EXPECT_TRUE(set.empty());
  EXPECT_EQ("", set.str());
}

// -- Abnormal cases: invalid grammar rejected --------------------------------

TEST(SequenceSet, RejectsEmptyString) {
  EXPECT_FALSE(sequence_set::is_valid(""));
}

TEST(SequenceSet, RejectsZero) {
  // nz-number: a sequence number may not be zero.
  EXPECT_FALSE(sequence_set::is_valid("0"));
  EXPECT_FALSE(sequence_set::is_valid("0:5"));
  EXPECT_FALSE(sequence_set::is_valid("1:0"));
}

TEST(SequenceSet, RejectsLeadingZeros) {
  // nz-number starts with a non-zero digit; "01" is not a valid nz-number.
  EXPECT_FALSE(sequence_set::is_valid("01"));
  EXPECT_FALSE(sequence_set::is_valid("007"));
  EXPECT_FALSE(sequence_set::is_valid("01:5"));
}

TEST(SequenceSet, RejectsBadCharacters) {
  EXPECT_FALSE(sequence_set::is_valid("a"));
  EXPECT_FALSE(sequence_set::is_valid("1,a"));
  EXPECT_FALSE(sequence_set::is_valid("1;2"));
  EXPECT_FALSE(sequence_set::is_valid("1 2"));
  EXPECT_FALSE(sequence_set::is_valid("1:2:3"));
  EXPECT_FALSE(sequence_set::is_valid("-1"));
  EXPECT_FALSE(sequence_set::is_valid("+1"));
}

TEST(SequenceSet, RejectsStructuralSlips) {
  EXPECT_FALSE(sequence_set::is_valid(","));
  EXPECT_FALSE(sequence_set::is_valid("1,"));
  EXPECT_FALSE(sequence_set::is_valid(",1"));
  EXPECT_FALSE(sequence_set::is_valid(":"));
  EXPECT_FALSE(sequence_set::is_valid("1:"));
  EXPECT_FALSE(sequence_set::is_valid(":5"));
  EXPECT_FALSE(sequence_set::is_valid("1,,2"));
}

TEST(SequenceSet, ConstructorThrowsOnInvalidInput) {
  EXPECT_THROW((void)sequence_set(""), std::invalid_argument);
  EXPECT_THROW((void)sequence_set("0"), std::invalid_argument);
  EXPECT_THROW((void)sequence_set("01"), std::invalid_argument);
  EXPECT_THROW((void)sequence_set("4294967296"), std::invalid_argument);
  EXPECT_THROW((void)sequence_set("1:2:3"), std::invalid_argument);
}

TEST(SequenceSet, ParseReturnsNulloptOnInvalidInput) {
  EXPECT_EQ(std::nullopt, sequence_set::parse(""));
  EXPECT_EQ(std::nullopt, sequence_set::parse("0"));
  EXPECT_EQ(std::nullopt, sequence_set::parse("1,"));
  EXPECT_EQ(std::nullopt, sequence_set::parse("abc"));
}

}  // namespace
