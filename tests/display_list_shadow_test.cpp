#include "gx/display_list_shadow.hpp"
#include <gtest/gtest.h>
#include <array>
#include <algorithm>

namespace {
using aurora::gx::fifo::DisplayListShadowCache;
using namespace aurora::vita::gfx;

TEST(DisplayListShadow, UnchangedListReusesShadowWithoutPublishingWrites) {
  std::array<uint8_t, 32> list{{0x90, 0, 1, 0, 2}};
  note_memory_write(list.data(), list.size());
  DisplayListShadowCache cache;
  const auto* first = cache.get(list.data(), list.size());
  ASSERT_NE(first, nullptr);
  EXPECT_NE(first, list.data());
  const auto epoch = memory_write_epoch();
  EXPECT_EQ(cache.get(list.data(), list.size()), first);
  EXPECT_EQ(memory_write_epoch(), epoch);
  EXPECT_EQ(cache.untracked_writes(), 0u);
  EXPECT_EQ(cache.bytes(), list.size());
}

TEST(DisplayListShadow, DetectsUnflushedPnMatrixIndexAndInvalidatesGuestIdentity) {
  std::array<uint8_t, 32> list{{0x90, 0, 1, 0xff, 0, 2}};
  note_memory_write(list.data(), list.size());
  DisplayListShadowCache cache;
  ASSERT_NE(cache.get(list.data(), list.size()), nullptr);
  const auto revision = memory_range_revision(list.data() + 3, list.size() - 3);
  list[3] = 6; // Stitching without DCFlush, reproducing a revision-only false hit.
  EXPECT_EQ(memory_range_revision(list.data(), list.size()), revision);
  const auto* fresh = cache.get(list.data(), list.size());
  ASSERT_NE(fresh, nullptr);
  EXPECT_TRUE(byte_spans_equal(fresh, list.data(), list.size()));
  EXPECT_GT(memory_range_revision(list.data() + 3, list.size() - 3), revision);
  EXPECT_EQ(cache.untracked_writes(), 1u);
  EXPECT_EQ(cache.bytes(), list.size());
  EXPECT_EQ(cache.get(list.data(), list.size()), fresh);
  EXPECT_EQ(cache.untracked_writes(), 1u);
}

TEST(DisplayListShadow, DetectsUnnotifiedReuseOfSameAddressAndLengthIncludingPadding) {
  std::array<uint8_t, 32> list{{0x90, 0, 1, 0, 2}};
  DisplayListShadowCache cache;
  ASSERT_NE(cache.get(list.data(), list.size()), nullptr);
  std::fill(list.begin(), list.end(), 0);
  list[0] = 0x98;
  list.back() = 0x08;
  const auto* fresh = cache.get(list.data(), list.size());
  ASSERT_NE(fresh, nullptr);
  EXPECT_TRUE(byte_spans_equal(fresh, list.data(), list.size()));
  EXPECT_EQ(cache.untracked_writes(), 1u);
  list.back() = 0; // Check a mutation only in the last padding byte as well.
  fresh = cache.get(list.data(), list.size());
  ASSERT_NE(fresh, nullptr);
  EXPECT_TRUE(byte_spans_equal(fresh, list.data(), list.size()));
  EXPECT_EQ(cache.untracked_writes(), 2u);
}

TEST(DisplayListShadow, TrackedWriteAcrossPageBoundaryRefreshesWithoutFalseReport) {
  std::vector<uint8_t> storage(2 * 65536 + 32);
  const auto aligned = (reinterpret_cast<uintptr_t>(storage.data()) + 65535u) & ~uintptr_t(65535u);
  auto* list = reinterpret_cast<uint8_t*>(aligned + 65536 - 16);
  std::fill(list, list + 32, 0);
  note_memory_write(list, 32);
  DisplayListShadowCache cache;
  ASSERT_NE(cache.get(list, 32), nullptr);
  const auto revision = memory_range_revision(list, 32);
  list[31] = 3;
  note_memory_write(list + 31, 1);
  EXPECT_GT(memory_range_revision(list, 32), revision);
  const auto* fresh = cache.get(list, 32);
  ASSERT_NE(fresh, nullptr);
  EXPECT_TRUE(byte_spans_equal(fresh, list, 32));
  EXPECT_EQ(cache.untracked_writes(), 0u);
}

TEST(DisplayListShadow, LengthChangesBudgetEvictionAndClearPreserveFreshBytes) {
  std::array<uint8_t, 64> a{}, b{};
  a.fill(0x90);
  b.fill(0x98);
  DisplayListShadowCache cache(64);
  ASSERT_NE(cache.get(a.data(), 32), nullptr);
  EXPECT_EQ(cache.bytes(), 32u);
  ASSERT_NE(cache.get(a.data(), 64), nullptr);
  EXPECT_EQ(cache.bytes(), 64u);
  const auto* fresh = cache.get(b.data(), 32);
  ASSERT_NE(fresh, nullptr);
  EXPECT_TRUE(byte_spans_equal(fresh, b.data(), 32));
  EXPECT_EQ(cache.bytes(), 32u);
  fresh = cache.get(a.data(), 32);
  ASSERT_NE(fresh, nullptr);
  EXPECT_TRUE(byte_spans_equal(fresh, a.data(), 32));
  EXPECT_EQ(cache.bytes(), 64u);
  cache.clear();
  EXPECT_EQ(cache.bytes(), 0u);
  EXPECT_EQ(cache.untracked_writes(), 0u);
  a[3] ^= 1;
  fresh = cache.get(a.data(), 32);
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh[3], a[3]);
  EXPECT_EQ(cache.untracked_writes(), 0u);
  EXPECT_EQ(cache.get(nullptr, 32), nullptr);
  EXPECT_EQ(cache.get(a.data(), 0), nullptr);
  EXPECT_EQ(cache.get(a.data(), 65), nullptr);
  EXPECT_EQ(cache.bytes(), 32u);
}
} // namespace
