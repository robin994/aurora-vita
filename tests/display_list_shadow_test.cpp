#include "gx/display_list_shadow.hpp"
#include <gtest/gtest.h>
#include <array>
#include <algorithm>

namespace {
using aurora::gx::fifo::DisplayListShadowCache;
using namespace aurora::vita::gfx;

TEST(DisplayListShadow, NativeRecipeAllowsCachedRamWithoutChangingOrdinaryPolicy) {
  using aurora::gx::fifo::display_list_shadow_source_allowed;
  EXPECT_FALSE(display_list_shadow_source_allowed(0x8f800000u,false));
  EXPECT_TRUE(display_list_shadow_source_allowed(0x8f800000u,true));
  EXPECT_FALSE(display_list_shadow_source_allowed(0,false));
  EXPECT_FALSE(display_list_shadow_source_allowed(0,true));
  EXPECT_FALSE(display_list_shadow_source_allowed(0x5fffffffu,false));
  EXPECT_TRUE(display_list_shadow_source_allowed(0x60000000u,false));
  EXPECT_TRUE(display_list_shadow_source_allowed(0x6fffffffu,false));
  EXPECT_FALSE(display_list_shadow_source_allowed(0x70000000u,false));
}

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

TEST(DisplayListShadow, PinnedStorageSurvivesRefreshAndClear) {
  std::array<uint8_t, 32> list{{0x90, 0, 1, 0, 2}};
  note_memory_write(list.data(), list.size());
  DisplayListShadowCache cache;
  const auto pinned = cache.pin(list.data(), list.size());
  ASSERT_NE(pinned, nullptr);
  const auto original = *pinned;
  list[3] = 7;
  note_memory_write(list.data() + 3, 1);
  const auto refreshed = cache.pin(list.data(), list.size());
  ASSERT_NE(refreshed, nullptr);
  EXPECT_NE(refreshed.get(), pinned.get());
  EXPECT_EQ((*refreshed)[3], 7);
  EXPECT_EQ(*pinned, original);
  cache.clear();
  EXPECT_EQ(*pinned, original);
}

TEST(DisplayListShadow, RevisionOnlyModeReusesUnchangedRevision) {
  std::array<uint8_t, 32> list{{0x90, 0, 1, 0, 2}};
  note_memory_write(list.data(), list.size());
  DisplayListShadowCache cache(1024, false);
  const auto pinned = cache.pin(list.data(), list.size());
  ASSERT_NE(pinned, nullptr);
  EXPECT_EQ(cache.pin(list.data(), list.size()).get(), pinned.get());
  EXPECT_EQ(cache.untracked_writes(), 0u);
}

TEST(DisplayListShadow, NativePinValidatesUnnotifiedChangesAndKeepsQueuedBytesImmutable) {
  std::array<uint8_t,32> list{{0x90,0,1,0xff,0,2}};
  note_memory_write(list.data(),list.size());
  DisplayListShadowCache cache(1024,false);
  uint64_t oldIdentity=0,newIdentity=0;
  const auto queued=cache.pin(list.data(),list.size(),&oldIdentity,true);
  ASSERT_NE(queued,nullptr);
  const auto revision=memory_range_revision(list.data(),list.size());
  list[3]=6;
  EXPECT_EQ(memory_range_revision(list.data(),list.size()),revision);
  const auto current=cache.pin(list.data(),list.size(),&newIdentity,true);
  ASSERT_NE(current,nullptr);
  EXPECT_EQ((*queued)[3],0xff);
  EXPECT_TRUE(byte_spans_equal(current->data(),list.data(),list.size()));
  EXPECT_NE(newIdentity,oldIdentity);
  EXPECT_GT(memory_range_revision(list.data(),list.size()),revision);
  EXPECT_EQ(cache.bytes(),list.size());
}

TEST(DisplayListShadow, BatchedRevisionChecksObserveTrackedWrites) {
  std::array<uint8_t, 32> first{}, second{};
  note_memory_write(first.data(), first.size());
  note_memory_write(second.data(), second.size());
  std::array<MemoryRevisionCheck, 2> checks{{
      {first.data(), first.size(), memory_range_revision(first.data(), first.size())},
      {second.data(), second.size(), memory_range_revision(second.data(), second.size())},
  }};
  EXPECT_TRUE(memory_ranges_match(checks.data(), checks.size()));
  second[7] = 1;
  note_memory_write(second.data() + 7, 1);
  EXPECT_FALSE(memory_ranges_match(checks.data(), checks.size()));
  checks[0].revision = memory_range_revision(first.data(), first.size());
  checks[1].revision = memory_range_revision(second.data(), second.size());
  EXPECT_TRUE(memory_ranges_match(checks.data(), checks.size()));
}
} // namespace
