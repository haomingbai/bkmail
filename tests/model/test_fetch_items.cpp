/**
 * @file tests/model/test_fetch_items.cpp
 * @brief Unit tests for bkmail::imap::fetch_items and store_mode.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/fetch_items.h>
#include <gtest/gtest.h>

#include <type_traits>

namespace {

using bkmail::imap::fetch_items;
using bkmail::imap::store_mode;

// -- Normal cases ------------------------------------------------------------

TEST(FetchItems, DefaultSelectionIsEmpty) {
  const fetch_items items;
  EXPECT_FALSE(items.any());
  EXPECT_TRUE(items.sections().empty());
  for (const auto i :
       {fetch_items::item::envelope, fetch_items::item::body_structure,
        fetch_items::item::flags, fetch_items::item::uid,
        fetch_items::item::internal_date, fetch_items::item::rfc822_size}) {
    EXPECT_FALSE(items.test(i));
  }
}

TEST(FetchItems, AddSetsExactlyOneBit) {
  fetch_items items;
  items.add(fetch_items::item::envelope);
  EXPECT_TRUE(items.test(fetch_items::item::envelope));
  EXPECT_FALSE(items.test(fetch_items::item::uid));
  EXPECT_TRUE(items.any());
}

TEST(FetchItems, BitmaskCombinationsAccumulate) {
  fetch_items items;
  items.add(fetch_items::item::envelope)
      .add(fetch_items::item::flags)
      .add(fetch_items::item::uid);
  EXPECT_TRUE(items.test(fetch_items::item::envelope));
  EXPECT_TRUE(items.test(fetch_items::item::flags));
  EXPECT_TRUE(items.test(fetch_items::item::uid));
  EXPECT_FALSE(items.test(fetch_items::item::body_structure));
  EXPECT_FALSE(items.test(fetch_items::item::internal_date));
  EXPECT_FALSE(items.test(fetch_items::item::rfc822_size));
}

TEST(FetchItems, RemoveClearsOnlyTheNamedBit) {
  fetch_items items;
  items.add(fetch_items::item::envelope).add(fetch_items::item::uid);
  items.remove(fetch_items::item::envelope);
  EXPECT_FALSE(items.test(fetch_items::item::envelope));
  EXPECT_TRUE(items.test(fetch_items::item::uid));
  EXPECT_TRUE(items.any());
}

TEST(FetchItems, ClearResetsBitsAndSections) {
  fetch_items items;
  items.add(fetch_items::item::flags).add_section("HEADER");
  items.clear();
  EXPECT_FALSE(items.any());
  EXPECT_FALSE(items.test(fetch_items::item::flags));
  EXPECT_TRUE(items.sections().empty());
}

TEST(FetchItems, SectionsKeepSpecifierAndPeekFlag) {
  fetch_items items;
  items.add_section("HEADER.FIELDS (FROM TO)");  // peek defaults to true
  items.add_section("TEXT", false);
  items.add_section("");  // BODY[]
  const auto& sections = items.sections();
  ASSERT_EQ(3U, sections.size());
  EXPECT_EQ("HEADER.FIELDS (FROM TO)", sections[0].specifier);
  EXPECT_TRUE(sections[0].peek);
  EXPECT_EQ("TEXT", sections[1].specifier);
  EXPECT_FALSE(sections[1].peek);
  EXPECT_EQ("", sections[2].specifier);
  EXPECT_TRUE(sections[2].peek);
}

TEST(FetchItems, SectionAloneMakesAnyTrue) {
  fetch_items items;
  items.add_section("HEADER");
  EXPECT_TRUE(items.any());
}

// -- Boundary cases ----------------------------------------------------------

TEST(FetchItems, AllSixItemsCoexist) {
  fetch_items items;
  for (const auto i :
       {fetch_items::item::envelope, fetch_items::item::body_structure,
        fetch_items::item::flags, fetch_items::item::uid,
        fetch_items::item::internal_date, fetch_items::item::rfc822_size}) {
    items.add(i);
  }
  for (const auto i :
       {fetch_items::item::envelope, fetch_items::item::body_structure,
        fetch_items::item::flags, fetch_items::item::uid,
        fetch_items::item::internal_date, fetch_items::item::rfc822_size}) {
    EXPECT_TRUE(items.test(i));
  }
}

TEST(FetchItems, ItemBitsAreDistinctPowersOfTwo) {
  using U = std::underlying_type_t<fetch_items::item>;
  const U all[] = {static_cast<U>(fetch_items::item::envelope),
                   static_cast<U>(fetch_items::item::body_structure),
                   static_cast<U>(fetch_items::item::flags),
                   static_cast<U>(fetch_items::item::uid),
                   static_cast<U>(fetch_items::item::internal_date),
                   static_cast<U>(fetch_items::item::rfc822_size)};
  for (const U bit : all) {
    EXPECT_NE(0U, bit);
    EXPECT_EQ(0U, bit & (bit - 1U));  // power of two
  }
  U combined = 0;
  for (const U bit : all) {
    EXPECT_EQ(0U, combined & bit);  // no overlap
    combined |= bit;
  }
}

TEST(FetchItems, AddSameItemTwiceIsIdempotent) {
  fetch_items items;
  items.add(fetch_items::item::uid).add(fetch_items::item::uid);
  EXPECT_TRUE(items.test(fetch_items::item::uid));
  items.remove(fetch_items::item::uid);
  EXPECT_FALSE(items.any());
}

// -- store_mode --------------------------------------------------------------

TEST(StoreMode, HasAddRemoveReplace) {
  static_assert(std::is_same_v<store_mode, bkmail::imap::store_mode>);
  const store_mode modes[] = {store_mode::add, store_mode::remove,
                              store_mode::replace};
  EXPECT_EQ(3U, std::size(modes));
  EXPECT_NE(store_mode::add, store_mode::remove);
  EXPECT_NE(store_mode::remove, store_mode::replace);
  EXPECT_NE(store_mode::add, store_mode::replace);
}

}  // namespace
