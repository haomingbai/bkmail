/**
 * @file tests/model/test_mailbox.cpp
 * @brief Unit tests for mailbox_info, mailbox_entry, mailbox_status and
 * status_items.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/mailbox_entry.h>
#include <bkmail/imap/mailbox_info.h>
#include <bkmail/imap/mailbox_status.h>
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>

namespace {

using bkmail::imap::flag_set;
using bkmail::imap::mailbox_entry;
using bkmail::imap::mailbox_info;
using bkmail::imap::mailbox_status;
using bkmail::imap::message_flag;
using bkmail::imap::status_items;

// -- status_items bitmask ----------------------------------------------------

TEST(StatusItems, NoneHasNoBits) {
  EXPECT_EQ(status_items::none, status_items::none & status_items::messages);
  EXPECT_FALSE(bkmail::imap::has_status_item(status_items::none,
                                             status_items::messages));
}

TEST(StatusItems, OrCombinesMasks) {
  const status_items combined = status_items::messages | status_items::uid_next;
  EXPECT_TRUE(bkmail::imap::has_status_item(combined, status_items::messages));
  EXPECT_TRUE(bkmail::imap::has_status_item(combined, status_items::uid_next));
  EXPECT_FALSE(bkmail::imap::has_status_item(combined, status_items::recent));
}

TEST(StatusItems, OrEqualsAccumulates) {
  status_items items = status_items::none;
  items |= status_items::recent;
  items |= status_items::unseen;
  EXPECT_TRUE(bkmail::imap::has_status_item(items, status_items::recent));
  EXPECT_TRUE(bkmail::imap::has_status_item(items, status_items::unseen));
  EXPECT_EQ(2, std::popcount(static_cast<unsigned char>(items)));
}

TEST(StatusItems, AndIntersectsMasks) {
  const status_items a = status_items::messages | status_items::recent;
  const status_items b = status_items::recent | status_items::unseen;
  const status_items both = a & b;
  EXPECT_TRUE(bkmail::imap::has_status_item(both, status_items::recent));
  EXPECT_FALSE(bkmail::imap::has_status_item(both, status_items::messages));
  EXPECT_FALSE(bkmail::imap::has_status_item(both, status_items::unseen));
}

TEST(StatusItems, HasStatusItemRequiresEveryFlagOfTheArgument) {
  const status_items pair = status_items::messages | status_items::recent;
  EXPECT_TRUE(bkmail::imap::has_status_item(
      pair, status_items::messages | status_items::recent));
  EXPECT_FALSE(bkmail::imap::has_status_item(
      pair, status_items::messages | status_items::unseen));
  // An empty flag set is trivially contained.
  EXPECT_TRUE(bkmail::imap::has_status_item(pair, status_items::none));
}

TEST(StatusItems, AllFiveItemsAreDistinctBits) {
  const status_items all[] = {status_items::messages, status_items::recent,
                              status_items::uid_next,
                              status_items::uid_validity, status_items::unseen};
  unsigned seen = 0;
  for (const auto item : all) {
    const auto bit = static_cast<unsigned>(item);
    EXPECT_NE(0U, bit);
    EXPECT_EQ(0U, seen & bit);
    seen |= bit;
  }
  EXPECT_EQ(5, std::popcount(seen));
}

TEST(StatusItems, IsConstexprFriendly) {
  constexpr status_items items =
      status_items::messages | status_items::uid_validity;
  static_assert(bkmail::imap::has_status_item(items, status_items::messages));
  static_assert(!bkmail::imap::has_status_item(items, status_items::unseen));
  SUCCEED();
}

// -- mailbox_info ------------------------------------------------------------

TEST(MailboxInfo, DefaultsAreZeroAndDisengaged) {
  const mailbox_info<> info;
  EXPECT_TRUE(info.name.empty());
  EXPECT_EQ(0U, info.exists);
  EXPECT_EQ(0U, info.recent);
  EXPECT_FALSE(info.unseen.has_value());
  EXPECT_EQ(0U, info.uid_validity);
  EXPECT_EQ(0U, info.uid_next);
  EXPECT_FALSE(info.flags.any());
  EXPECT_FALSE(info.permanent_flags.any());
  EXPECT_FALSE(info.read_only);
}

TEST(MailboxInfo, UnseenDistinguishesAbsentFromZero) {
  // unseen is optional precisely because "server sent no UNSEEN code" is
  // not the same as "first unseen is 0" (which the wire never sends).
  mailbox_info<> info;
  EXPECT_FALSE(info.unseen.has_value());
  info.unseen = 17;
  ASSERT_TRUE(info.unseen.has_value());
  EXPECT_EQ(17U, *info.unseen);
}

TEST(MailboxInfo, FieldsAreAssignable) {
  mailbox_info<> info;
  info.name = "INBOX";
  info.exists = 3;
  info.recent = 1;
  info.uid_validity = 42;
  info.uid_next = 100;
  info.flags.set(message_flag::seen).set(message_flag::answered);
  info.permanent_flags.set(message_flag::seen);
  info.read_only = true;

  EXPECT_EQ("INBOX", info.name);
  EXPECT_EQ(3U, info.exists);
  EXPECT_EQ(1U, info.recent);
  EXPECT_EQ(42U, info.uid_validity);
  EXPECT_EQ(100U, info.uid_next);
  EXPECT_TRUE(info.flags.test(message_flag::seen));
  EXPECT_TRUE(info.flags.test(message_flag::answered));
  EXPECT_TRUE(info.permanent_flags.test(message_flag::seen));
  EXPECT_FALSE(info.permanent_flags.test(message_flag::answered));
  EXPECT_TRUE(info.read_only);
}

// -- mailbox_entry -----------------------------------------------------------

TEST(MailboxEntry, DefaultsAreEmptyAndFalse) {
  const mailbox_entry<> entry;
  EXPECT_TRUE(entry.name.empty());
  EXPECT_TRUE(entry.delimiter.empty());
  EXPECT_FALSE(entry.no_select);
  EXPECT_FALSE(entry.has_children);
  EXPECT_FALSE(entry.has_no_children);
}

TEST(MailboxEntry, NilDelimiterIsAnEmptyString) {
  const mailbox_entry<> entry{.name = "INBOX"};
  EXPECT_EQ("INBOX", entry.name);
  EXPECT_EQ("", entry.delimiter);
}

TEST(MailboxEntry, AttributesRoundTrip) {
  const mailbox_entry<> entry{.name = "Archive/2026",
                              .delimiter = "/",
                              .no_select = true,
                              .has_children = true};
  EXPECT_EQ("Archive/2026", entry.name);
  EXPECT_EQ("/", entry.delimiter);
  EXPECT_TRUE(entry.no_select);
  EXPECT_TRUE(entry.has_children);
  EXPECT_FALSE(entry.has_no_children);
}

// -- mailbox_status ----------------------------------------------------------

TEST(MailboxStatus, EveryFieldIsDisengagedByDefault) {
  const mailbox_status<> status;
  EXPECT_FALSE(status.messages.has_value());
  EXPECT_FALSE(status.recent.has_value());
  EXPECT_FALSE(status.uid_next.has_value());
  EXPECT_FALSE(status.uid_validity.has_value());
  EXPECT_FALSE(status.unseen.has_value());
}

TEST(MailboxStatus, EngagedFieldsHoldValues) {
  mailbox_status<> status;
  status.messages = 10;
  status.unseen = 2;
  EXPECT_EQ(10U, *status.messages);
  EXPECT_EQ(2U, *status.unseen);
  // Unrequested items stay disengaged even when others are filled.
  EXPECT_FALSE(status.recent.has_value());
  EXPECT_FALSE(status.uid_next.has_value());
  EXPECT_FALSE(status.uid_validity.has_value());
}

TEST(MailboxStatus, EngagedZeroDiffersFromDisengaged) {
  // A reported zero (e.g. no unseen mail) must be distinguishable from
  // "not requested".
  mailbox_status<> status;
  status.unseen = 0;
  ASSERT_TRUE(status.unseen.has_value());
  EXPECT_EQ(0U, *status.unseen);
  EXPECT_FALSE(status.recent.has_value());
}

}  // namespace
