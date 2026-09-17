/**
 * @file tests/model/test_flags.cpp
 * @brief Unit tests for bkmail::imap::message_flag and flag_set.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/flags.h>
#include <gtest/gtest.h>

#include <type_traits>

namespace {

using bkmail::imap::flag_set;
using bkmail::imap::message_flag;

// -- Normal cases ------------------------------------------------------------

TEST(FlagSet, DefaultConstructedHasNoFlags) {
  constexpr flag_set flags;
  EXPECT_FALSE(flags.any());
  for (const auto f :
       {message_flag::seen, message_flag::answered, message_flag::flagged,
        message_flag::deleted, message_flag::draft, message_flag::recent}) {
    EXPECT_FALSE(flags.test(f));
  }
}

TEST(FlagSet, SetEnablesExactlyOneFlag) {
  flag_set flags;
  flags.set(message_flag::seen);
  EXPECT_TRUE(flags.test(message_flag::seen));
  EXPECT_FALSE(flags.test(message_flag::answered));
  EXPECT_TRUE(flags.any());
}

TEST(FlagSet, SetIsChainableAndAccumulates) {
  flag_set flags;
  flags.set(message_flag::seen)
      .set(message_flag::deleted)
      .set(message_flag::draft);
  EXPECT_TRUE(flags.test(message_flag::seen));
  EXPECT_TRUE(flags.test(message_flag::deleted));
  EXPECT_TRUE(flags.test(message_flag::draft));
  EXPECT_FALSE(flags.test(message_flag::flagged));
}

TEST(FlagSet, ResetClearsOnlyTheNamedFlag) {
  flag_set flags;
  flags.set(message_flag::seen).set(message_flag::answered);
  flags.reset(message_flag::seen);
  EXPECT_FALSE(flags.test(message_flag::seen));
  EXPECT_TRUE(flags.test(message_flag::answered));
}

TEST(FlagSet, SetWithFalseValueClears) {
  flag_set flags;
  flags.set(message_flag::recent);
  flags.set(message_flag::recent, false);
  EXPECT_FALSE(flags.test(message_flag::recent));
  EXPECT_FALSE(flags.any());
}

TEST(FlagSet, EverySystemFlagIsIndependent) {
  // The six RFC 3501 system flags each occupy their own bit.
  for (const auto f :
       {message_flag::seen, message_flag::answered, message_flag::flagged,
        message_flag::deleted, message_flag::draft, message_flag::recent}) {
    flag_set flags;
    flags.set(f);
    EXPECT_TRUE(flags.test(f));
    EXPECT_TRUE(flags.any());
    for (const auto g :
         {message_flag::seen, message_flag::answered, message_flag::flagged,
          message_flag::deleted, message_flag::draft, message_flag::recent}) {
      if (g != f) EXPECT_FALSE(flags.test(g));
    }
  }
}

TEST(FlagSet, EqualityReflectsBits) {
  flag_set a;
  flag_set b;
  EXPECT_EQ(a, b);
  a.set(message_flag::flagged);
  EXPECT_NE(a, b);
  b.set(message_flag::flagged);
  EXPECT_EQ(a, b);
}

// -- Boundary cases ----------------------------------------------------------

TEST(FlagSet, AllSixFlagsCoexist) {
  flag_set flags;
  for (const auto f :
       {message_flag::seen, message_flag::answered, message_flag::flagged,
        message_flag::deleted, message_flag::draft, message_flag::recent}) {
    flags.set(f);
  }
  for (const auto f :
       {message_flag::seen, message_flag::answered, message_flag::flagged,
        message_flag::deleted, message_flag::draft, message_flag::recent}) {
    EXPECT_TRUE(flags.test(f));
  }
  EXPECT_TRUE(flags.any());
}

TEST(FlagSet, HighestBitFlagDoesNotBleed) {
  // message_flag::recent is the sixth flag (bit 1 << 5); setting it must
  // not touch lower bits nor overflow the byte.
  flag_set flags;
  flags.set(message_flag::recent);
  EXPECT_EQ(flag_set{}.set(message_flag::recent), flags);
  flags.reset(message_flag::recent);
  EXPECT_EQ(flag_set{}, flags);
}

TEST(FlagSet, RepeatedSetAndResetAreIdempotent) {
  flag_set flags;
  flags.set(message_flag::seen).set(message_flag::seen);
  EXPECT_TRUE(flags.test(message_flag::seen));
  flags.reset(message_flag::seen).reset(message_flag::seen);
  EXPECT_FALSE(flags.test(message_flag::seen));
}

TEST(FlagSet, IsUsableInConstantEvaluation) {
  constexpr flag_set flags = [] {
    flag_set f;
    f.set(message_flag::answered);
    return f;
  }();
  static_assert(flags.test(message_flag::answered));
  static_assert(!flags.test(message_flag::seen));
  static_assert(flags.any());
  SUCCEED();
}

// -- Abnormal-ish cases ------------------------------------------------------

TEST(FlagSet, ResetOnEmptySetKeepsItEmpty) {
  flag_set flags;
  flags.reset(message_flag::deleted);
  EXPECT_FALSE(flags.any());
  EXPECT_EQ(flag_set{}, flags);
}

TEST(FlagSet, TestOnDefaultSetIsFalseForEveryFlag) {
  const flag_set flags;
  EXPECT_FALSE(flags.test(message_flag::draft));
  EXPECT_FALSE(flags.test(message_flag::recent));
}

}  // namespace
