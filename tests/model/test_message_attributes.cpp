/**
 * @file tests/model/test_message_attributes.cpp
 * @brief Unit tests for bkmail::imap::message_attributes.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/message_attributes.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

using bkmail::imap::message_attributes;
using bkmail::imap::message_flag;

// -- Normal cases ------------------------------------------------------------

TEST(MessageAttributes, ScalarDefaultsMeanAbsent) {
  const message_attributes<> attrs;
  EXPECT_FALSE(attrs.flags.any());
  EXPECT_TRUE(attrs.internal_date.empty());
  EXPECT_EQ(0U, attrs.rfc822_size);
  EXPECT_EQ(0U, attrs.uid);
  EXPECT_FALSE(attrs.envelope.has_value());
  EXPECT_FALSE(attrs.body_structure.has_value());
  EXPECT_TRUE(attrs.sections.empty());
}

TEST(MessageAttributes, ScalarFieldsAreFillable) {
  message_attributes<> attrs;
  attrs.flags.set(message_flag::seen);
  attrs.internal_date = " 7-Sep-2026 10:00:00 +0000";
  attrs.rfc822_size = 1234;
  attrs.uid = 77;

  EXPECT_TRUE(attrs.flags.test(message_flag::seen));
  EXPECT_EQ(" 7-Sep-2026 10:00:00 +0000", attrs.internal_date);
  EXPECT_EQ(1234U, attrs.rfc822_size);
  EXPECT_EQ(77U, attrs.uid);
}

TEST(MessageAttributes, EnvelopeEmbedsAsOptional) {
  message_attributes<> attrs;
  EXPECT_FALSE(attrs.envelope.has_value());
  attrs.envelope.emplace();
  attrs.envelope->subject = "wire subject";
  attrs.envelope->from.push_back(
      {.mailbox_name = "alice", .host_name = "x.dev"});
  ASSERT_TRUE(attrs.envelope.has_value());
  EXPECT_EQ("wire subject", attrs.envelope->subject);
  ASSERT_EQ(1U, attrs.envelope->from.size());
  EXPECT_EQ("alice@x.dev", attrs.envelope->from[0].email());
}

TEST(MessageAttributes, BodyStructureEmbedsAsOptional) {
  message_attributes<> attrs;
  EXPECT_FALSE(attrs.body_structure.has_value());
  attrs.body_structure.emplace();
  attrs.body_structure->media_type = "text";
  attrs.body_structure->subtype = "html";
  attrs.body_structure->octets = 512;
  ASSERT_TRUE(attrs.body_structure.has_value());
  EXPECT_EQ("text", attrs.body_structure->media_type);
  EXPECT_EQ("html", attrs.body_structure->subtype);
  EXPECT_EQ(512U, attrs.body_structure->octets);
}

TEST(MessageAttributes, SectionsStoreSpecifierAndOctetsInOrder) {
  message_attributes<> attrs;
  attrs.sections.push_back(
      {.specifier = "HEADER", .data = {std::byte{'F'}, std::byte{':'}}});
  attrs.sections.push_back(
      {.specifier = "", .data = {std::byte{'x'}}});  // BODY[]
  ASSERT_EQ(2U, attrs.sections.size());
  EXPECT_EQ("HEADER", attrs.sections[0].specifier);
  ASSERT_EQ(2U, attrs.sections[0].data.size());
  EXPECT_EQ(std::byte{'F'}, attrs.sections[0].data[0]);
  EXPECT_EQ("", attrs.sections[1].specifier);
  ASSERT_EQ(1U, attrs.sections[1].data.size());
}

// -- Boundary cases ----------------------------------------------------------

TEST(MessageAttributes, Rfc822SizeIs64Bit) {
  static_assert(std::is_same_v<std::uint64_t,
                               decltype(message_attributes<>::rfc822_size)>);
  message_attributes<> attrs;
  attrs.rfc822_size = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), attrs.rfc822_size);
}

TEST(MessageAttributes, UidIs32Bit) {
  static_assert(
      std::is_same_v<std::uint32_t, decltype(message_attributes<>::uid)>);
  message_attributes<> attrs;
  attrs.uid = std::numeric_limits<std::uint32_t>::max();
  EXPECT_EQ(std::numeric_limits<std::uint32_t>::max(), attrs.uid);
}

TEST(MessageAttributes, EmbeddedOptionalsResetIndependently) {
  message_attributes<> attrs;
  attrs.envelope.emplace();
  attrs.body_structure.emplace();
  attrs.envelope.reset();
  EXPECT_FALSE(attrs.envelope.has_value());
  EXPECT_TRUE(attrs.body_structure.has_value());
}

}  // namespace
