/**
 * @file tests/model/test_capability_set.cpp
 * @brief Unit tests for bkmail::imap::capability_set.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/imap/capability_set.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <vector>

namespace {

using bkmail::imap::capability_set;

// -- Normal cases ------------------------------------------------------------

TEST(CapabilitySet, DefaultConstructedIsEmpty) {
  const capability_set<> caps;
  EXPECT_TRUE(caps.empty());
  EXPECT_EQ(0U, caps.size());
  EXPECT_EQ(caps.begin(), caps.end());
}

TEST(CapabilitySet, InsertAddsCapabilityNormalizedToUppercase) {
  capability_set<> caps;
  caps.insert("uidplus");
  ASSERT_EQ(1U, caps.size());
  EXPECT_EQ("UIDPLUS", *caps.begin());
}

TEST(CapabilitySet, ContainsIsCaseInsensitive) {
  capability_set<> caps;
  caps.insert("Literal+");
  EXPECT_TRUE(caps.contains("LITERAL+"));
  EXPECT_TRUE(caps.contains("literal+"));
  EXPECT_TRUE(caps.contains("LiTeRaL+"));
}

TEST(CapabilitySet, ContainsReportsAbsence) {
  capability_set<> caps;
  caps.insert("IDLE");
  EXPECT_FALSE(caps.contains("UIDPLUS"));
  EXPECT_FALSE(caps.contains(""));
  EXPECT_FALSE(caps.contains("IDL"));
  EXPECT_FALSE(caps.contains("IDLE2"));
}

TEST(CapabilitySet, IterationVisitsEveryCapabilityInInsertOrder) {
  capability_set<> caps;
  caps.insert("imap4rev2");
  caps.insert("SASL-IR");
  caps.insert("idle");
  std::vector<std::string> seen;
  for (const auto& cap : caps) {
    seen.emplace_back(cap.data(), cap.size());
  }
  EXPECT_EQ((std::vector<std::string>{"IMAP4REV2", "SASL-IR", "IDLE"}), seen);
}

TEST(CapabilitySet, ClearEmptiesTheSet) {
  capability_set<> caps;
  caps.insert("UIDPLUS");
  caps.insert("IDLE");
  caps.clear();
  EXPECT_TRUE(caps.empty());
  EXPECT_EQ(0U, caps.size());
  EXPECT_FALSE(caps.contains("UIDPLUS"));
}

// -- Boundary cases ----------------------------------------------------------

TEST(CapabilitySet, InsertDeduplicatesCaseInsensitively) {
  capability_set<> caps;
  caps.insert("UIDPLUS");
  caps.insert("uidplus");
  caps.insert("UidPlus");
  EXPECT_EQ(1U, caps.size());
}

TEST(CapabilitySet, InsertIgnoresEmptyName) {
  capability_set<> caps;
  caps.insert("");
  EXPECT_TRUE(caps.empty());
  EXPECT_EQ(0U, caps.size());
}

TEST(CapabilitySet, DistinctCapabilitiesAllSurvive) {
  capability_set<> caps;
  for (const auto* name :
       {"IMAP4rev1", "STARTTLS", "LOGINDISABLED", "SASL-IR", "LITERAL+", "IDLE",
        "UIDPLUS", "MOVE", "X-GM-EXT-1"}) {
    caps.insert(name);
  }
  EXPECT_EQ(9U, caps.size());
  for (const auto* name :
       {"imap4REV1", "starttls", "logindisabled", "sasl-ir", "literal+", "idle",
        "uidplus", "move", "x-gm-ext-1"}) {
    EXPECT_TRUE(caps.contains(name));
  }
}

TEST(CapabilitySet, NonAsciiBytesAreNotCaseFolded) {
  // Case folding is ASCII-only; a byte like 0xE4 must stay as-is and must
  // not accidentally compare equal to anything else.
  capability_set<> caps;
  const std::string name = std::string("X-") + static_cast<char>(0xE4);
  caps.insert(name);
  EXPECT_TRUE(caps.contains(name));
  EXPECT_EQ(1U, caps.size());
  EXPECT_FALSE(caps.contains("X-"));
}

// -- Allocator awareness (pmr smoke) -----------------------------------------

TEST(CapabilitySet, WorksWithPolymorphicAllocator) {
  char buffer[4096];
  std::pmr::monotonic_buffer_resource resource{buffer, sizeof(buffer)};
  std::pmr::polymorphic_allocator<std::byte> alloc{&resource};

  capability_set<std::pmr::polymorphic_allocator<std::byte>> caps{alloc};
  caps.insert("uidplus");
  caps.insert("Idle");
  caps.insert("UIDPLUS");  // duplicate, ignored

  EXPECT_EQ(2U, caps.size());
  EXPECT_TRUE(caps.contains("UIDPLUS"));
  EXPECT_TRUE(caps.contains("idle"));
  EXPECT_EQ(alloc, caps.get_allocator());
}

}  // namespace
