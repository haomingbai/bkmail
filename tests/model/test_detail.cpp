/**
 * @file tests/model/test_detail.cpp
 * @brief Unit tests for detail::unique_function, detail::allocator_ext
 * aliases, and the bkmail::pack sender adaptor.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-16
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/common/detail/allocator_ext.h>
#include <bkmail/common/detail/unique_function.h>
#include <bkmail/common/pack.h>
#include <gtest/gtest.h>

#include <bexec/just.hpp>
#include <bexec/sync_wait.hpp>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {

// -- detail::unique_function -------------------------------------------------

TEST(UniqueFunction, DefaultConstructedIsEmpty) {
  const bkmail::detail::unique_function<void(int)> fn;
  EXPECT_FALSE(static_cast<bool>(fn));
}

TEST(UniqueFunction, NullptrConstructedIsEmpty) {
  const bkmail::detail::unique_function<void()> fn{nullptr};
  EXPECT_FALSE(static_cast<bool>(fn));
}

TEST(UniqueFunction, StoresAndInvokesLambda) {
  bkmail::detail::unique_function<int(int, int)> add{
      [](int a, int b) { return a + b; }};
  ASSERT_TRUE(static_cast<bool>(add));
  EXPECT_EQ(7, add(3, 4));
}

TEST(UniqueFunction, StoresMoveOnlyCallable) {
  auto resource = std::make_unique<int>(41);
  bkmail::detail::unique_function<int()> fn{
      [ptr = std::move(resource)] { return *ptr + 1; }};
  EXPECT_EQ(42, fn());
}

TEST(UniqueFunction, MoveConstructionTransfersOwnership) {
  bkmail::detail::unique_function<int()> source{[] { return 5; }};
  bkmail::detail::unique_function<int()> target{std::move(source)};
  EXPECT_FALSE(static_cast<bool>(source));
  ASSERT_TRUE(static_cast<bool>(target));
  EXPECT_EQ(5, target());
}

TEST(UniqueFunction, MoveAssignmentTransfersAndReleases) {
  bool overwritten_target_destroyed = false;
  struct probe {
    bool* flag;
    explicit probe(bool* f) : flag(f) {}
    probe(probe&&) = default;
    ~probe() { *flag = true; }
    void operator()() const {}
  };
  probe p{&overwritten_target_destroyed};
  bkmail::detail::unique_function<void()> first{std::move(p)};
  bkmail::detail::unique_function<void()> second{[] {}};
  first = std::move(second);
  // The move-assigned-from wrapper is empty; the overwritten target was
  // destroyed.
  EXPECT_TRUE(overwritten_target_destroyed);
  EXPECT_FALSE(static_cast<bool>(second));
  EXPECT_TRUE(static_cast<bool>(first));
}

TEST(UniqueFunction, IsNotCopyable) {
  static_assert(
      !std::is_copy_constructible_v<bkmail::detail::unique_function<void()>>);
  static_assert(
      !std::is_copy_assignable_v<bkmail::detail::unique_function<void()>>);
  SUCCEED();
}

TEST(UniqueFunction, ResetLeavesItEmpty) {
  bkmail::detail::unique_function<void()> fn{[] {}};
  fn.reset();
  EXPECT_FALSE(static_cast<bool>(fn));
}

TEST(UniqueFunction, NullptrAssignmentClears) {
  bkmail::detail::unique_function<void()> fn{[] {}};
  fn = nullptr;
  EXPECT_FALSE(static_cast<bool>(fn));
}

TEST(UniqueFunction, SwapExchangesTargets) {
  bkmail::detail::unique_function<int()> a{[] { return 1; }};
  bkmail::detail::unique_function<int()> b{[] { return 2; }};
  a.swap(b);
  EXPECT_EQ(2, a());
  EXPECT_EQ(1, b());
  swap(a, b);
  EXPECT_EQ(1, a());
  EXPECT_EQ(2, b());
}

TEST(UniqueFunction, VoidAndNonVoidSignaturesWork) {
  int observed = 0;
  bkmail::detail::unique_function<void(int)> sink{
      [&observed](int v) { observed = v; }};
  sink(9);
  EXPECT_EQ(9, observed);
}

// -- detail::allocator_ext ---------------------------------------------------

TEST(AllocatorExt, RebindAllocProducesMatchingAllocator) {
  static_assert(std::is_same_v<
                bkmail::detail::rebind_alloc_t<std::allocator<std::byte>, char>,
                std::allocator<char>>);
  static_assert(
      std::is_same_v<bkmail::detail::rebind_alloc_t<
                         std::pmr::polymorphic_allocator<std::byte>, char>,
                     std::pmr::polymorphic_allocator<char>>);
  SUCCEED();
}

TEST(AllocatorExt, StringOfIsACharBasicString) {
  static_assert(
      std::is_same_v<bkmail::detail::string_of<std::allocator<std::byte>>,
                     std::string>);
  using pmr_string =
      bkmail::detail::string_of<std::pmr::polymorphic_allocator<std::byte>>;
  static_assert(std::is_same_v<pmr_string, std::pmr::string>);
  SUCCEED();
}

TEST(AllocatorExt, VectorOfRebindsTheValueType) {
  static_assert(
      std::is_same_v<bkmail::detail::vector_of<int, std::allocator<std::byte>>,
                     std::vector<int>>);
  static_assert(
      std::is_same_v<bkmail::detail::vector_of<
                         int, std::pmr::polymorphic_allocator<std::byte>>,
                     std::pmr::vector<int>>);
  SUCCEED();
}

TEST(AllocatorExt, AliasesAreUsableAtRuntime) {
  bkmail::detail::string_of<std::allocator<std::byte>> text{"abc"};
  bkmail::detail::vector_of<int, std::allocator<std::byte>> numbers{1, 2, 3};
  EXPECT_EQ("abc", text);
  EXPECT_EQ(3U, numbers.size());
}

// -- bkmail::pack ------------------------------------------------------------

TEST(Pack, PacksMultiValueCompletionIntoOneTuple) {
  auto result = bexec::this_thread::sync_wait(
      bkmail::pack(bexec::just(std::error_code{}, 42, std::string{"state"})));
  ASSERT_TRUE(result.has_value());
  static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(*result)>> ==
                1U);
  const auto& packed = std::get<0>(*result);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(packed)>,
                               std::tuple<std::error_code, int, std::string>>);
  EXPECT_EQ(std::error_code{}, std::get<0>(packed));
  EXPECT_EQ(42, std::get<1>(packed));
  EXPECT_EQ("state", std::get<2>(packed));
}

TEST(Pack, SingleValueCompletionBecomesOneTuple) {
  auto result = bexec::this_thread::sync_wait(bkmail::pack(bexec::just(3.5)));
  ASSERT_TRUE(result.has_value());
  const auto& packed = std::get<0>(*result);
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(packed)>,
                               std::tuple<double>>);
  EXPECT_DOUBLE_EQ(3.5, std::get<0>(packed));
}

TEST(Pack, ZeroValueCompletionBecomesEmptyTuple) {
  auto result = bexec::this_thread::sync_wait(bkmail::pack(bexec::just()));
  ASSERT_TRUE(result.has_value());
  const auto& packed = std::get<0>(*result);
  static_assert(
      std::is_same_v<std::remove_cvref_t<decltype(packed)>, std::tuple<>>);
  (void)packed;
}

}  // namespace
