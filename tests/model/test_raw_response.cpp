/**
 * @file tests/model/test_raw_response.cpp
 * @brief Unit tests for bkmail::imap::raw_response.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/raw_response.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <utility>

namespace {

using bkmail::imap::raw_response;

// -- Normal cases ------------------------------------------------------------

TEST(RawResponse, AggregateInitializationFillsAllFields) {
  const raw_response<> response{
      .tag = "a0001", .ok = true, .text = "EXPUNGE completed"};
  EXPECT_EQ("a0001", response.tag);
  EXPECT_TRUE(response.ok);
  EXPECT_EQ("EXPUNGE completed", response.text);
}

TEST(RawResponse, OkFalseModelsNoOrBadReplies) {
  const raw_response<> response{
      .tag = "a0002", .ok = false, .text = "No such command"};
  EXPECT_EQ("a0002", response.tag);
  EXPECT_FALSE(response.ok);
  EXPECT_EQ("No such command", response.text);
}

// -- Boundary cases ----------------------------------------------------------

TEST(RawResponse, DefaultConstructedIsEmptyAndNotOk) {
  const raw_response<> response;
  EXPECT_TRUE(response.tag.empty());
  EXPECT_FALSE(response.ok);
  EXPECT_TRUE(response.text.empty());
}

TEST(RawResponse, EmptyTextIsAllowed) {
  const raw_response<> response{.tag = "t", .ok = true};
  EXPECT_EQ("t", response.tag);
  EXPECT_TRUE(response.ok);
  EXPECT_EQ("", response.text);
}

// -- Allocator awareness -----------------------------------------------------

TEST(RawResponse, WorksWithPolymorphicAllocator) {
  char buffer[1024];
  std::pmr::monotonic_buffer_resource resource{buffer, sizeof(buffer)};
  const std::pmr::polymorphic_allocator<std::byte> alloc{&resource};
  using pmr_response = raw_response<std::pmr::polymorphic_allocator<std::byte>>;
  // Move-construct the members so the pmr allocator's memory resource is
  // preserved (copy construction would reselect the default resource).
  pmr_response::string_type tag{"a0042", alloc};
  pmr_response::string_type text{"OK", alloc};
  const pmr_response response{
      .tag = std::move(tag), .ok = true, .text = std::move(text)};
  EXPECT_EQ("a0042", response.tag);
  EXPECT_TRUE(response.ok);
  EXPECT_EQ("OK", response.text);
  EXPECT_EQ(alloc, response.tag.get_allocator());
}

}  // namespace
