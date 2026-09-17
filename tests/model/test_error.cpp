/**
 * @file tests/model/test_error.cpp
 * @brief Unit tests for bkmail::errc and bkmail::error_category.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/error.h>
#include <gtest/gtest.h>

#include <string>
#include <system_error>
#include <type_traits>

namespace {

// -- Normal cases ------------------------------------------------------------

TEST(ErrorCategory, IsNamedBkmail) {
  EXPECT_EQ(std::string("bkmail"), bkmail::error_category().name());
}

TEST(ErrorCategory, IsASingleton) {
  EXPECT_EQ(&bkmail::error_category(), &bkmail::error_category());
}

TEST(ErrorCategory, EveryErrcHasANonEmptyMessage) {
  for (const auto e :
       {bkmail::errc::command_rejected, bkmail::errc::bad_command,
        bkmail::errc::server_bye, bkmail::errc::unexpected_response,
        bkmail::errc::capability_required}) {
    const std::error_code ec = bkmail::make_error_code(e);
    EXPECT_FALSE(ec.message().empty()) << static_cast<int>(e);
  }
}

TEST(ErrorCategory, MessagesAreDistinct) {
  EXPECT_NE(bkmail::make_error_code(bkmail::errc::command_rejected).message(),
            bkmail::make_error_code(bkmail::errc::bad_command).message());
  EXPECT_NE(
      bkmail::make_error_code(bkmail::errc::server_bye).message(),
      bkmail::make_error_code(bkmail::errc::unexpected_response).message());
}

TEST(MakeErrorCode, CarriesValueAndCategory) {
  const std::error_code ec = bkmail::make_error_code(bkmail::errc::server_bye);
  EXPECT_EQ(static_cast<int>(bkmail::errc::server_bye), ec.value());
  EXPECT_EQ(&bkmail::error_category(), &ec.category());
}

TEST(MakeErrorCode, ErrcConvertsImplicitlyToErrorCode) {
  // is_error_code_enum specialization enables the implicit conversion.
  static_assert(std::is_convertible_v<bkmail::errc, std::error_code>);
  const std::error_code ec = bkmail::errc::command_rejected;
  EXPECT_EQ(bkmail::make_error_code(bkmail::errc::command_rejected), ec);
}

// -- Boundary cases ----------------------------------------------------------

TEST(ErrorCode, FirstValueIsOne) {
  static_assert(static_cast<int>(bkmail::errc::command_rejected) == 1);
  SUCCEED();
}

TEST(ErrorCode, DistinctErrcValuesCompareUnequal) {
  EXPECT_NE(std::error_code(bkmail::errc::command_rejected),
            std::error_code(bkmail::errc::bad_command));
  EXPECT_NE(std::error_code(bkmail::errc::unexpected_response),
            std::error_code(bkmail::errc::capability_required));
}

TEST(ErrorCode, AllErrcCodesAreTruthy) {
  // Every defined errc is a real error: none may be zero.
  for (const auto e :
       {bkmail::errc::command_rejected, bkmail::errc::bad_command,
        bkmail::errc::server_bye, bkmail::errc::unexpected_response,
        bkmail::errc::capability_required}) {
    EXPECT_TRUE(static_cast<bool>(std::error_code(e)));
  }
}

// -- Abnormal cases ----------------------------------------------------------

TEST(ErrorCategory, IsNotTheGenericOrSystemCategory) {
  EXPECT_NE(&bkmail::error_category(), &std::generic_category());
  EXPECT_NE(&bkmail::error_category(), &std::system_category());
  // Same numeric value, different category: must not compare equal.
  const std::error_code generic_one(1, std::generic_category());
  EXPECT_NE(generic_one,
            bkmail::make_error_code(bkmail::errc::command_rejected));
}

TEST(ErrorCategory, EquivalentMatchesSameCategoryCodes) {
  const std::error_code ec = bkmail::make_error_code(bkmail::errc::bad_command);
  EXPECT_TRUE(ec == bkmail::errc::bad_command);
  EXPECT_FALSE(ec == bkmail::errc::server_bye);
}

}  // namespace
