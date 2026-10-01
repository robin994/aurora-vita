#include "gx_test_common.hpp"
#include "gx/bp_write_cache.hpp"
#include "__gx.h"

using aurora::gx::fifo::BpWriteCache;
namespace fifo = aurora::gx::fifo;
extern "C" void GXApplyBPReg(u8 reg, u32 value);

TEST(BpWriteCacheContract, MaskedWritesAreNeverElidedAndInvalidateTheirSlot) {
  BpWriteCache cache;
  EXPECT_TRUE(cache.should_emit(0xc008abcd)); // unknown incoming mask
  EXPECT_TRUE(cache.should_emit(0xc008abcd)); // establish a full write
  EXPECT_FALSE(cache.should_emit(0xc008abcd));
  EXPECT_TRUE(cache.should_emit(0xfe00000f));
  EXPECT_TRUE(cache.should_emit(0xc008abcd)); // must consume the one-shot mask
  EXPECT_TRUE(cache.should_emit(0xc008abcd)); // refresh the invalidated slot
  EXPECT_FALSE(cache.should_emit(0xc008abcd));
}

TEST(BpWriteCacheContract, ActionRegistersAndAliasedGenModeAlwaysRemainInStream) {
  BpWriteCache cache;
  for (uint32_t reg : {0x00u, 0x45u, 0x47u, 0x52u, 0x61u, 0x63u, 0x65u, 0x66u, 0xe0u, 0xfeu}) {
    for (int repeat = 0; repeat < 8; ++repeat) EXPECT_TRUE(cache.should_emit(reg << 24));
  }
}

TEST(BpWriteCacheContract, RepeatedMaterialSetupKeepsOnlyRequiredWrites) {
  BpWriteCache cache;
  uint32_t emitted = 0;
  for (uint32_t draw = 0; draw < 1000; ++draw) {
    for (uint32_t reg = 0xc0; reg <= 0xdf; ++reg) emitted += cache.should_emit((reg << 24) | 0x080000u);
  }
  EXPECT_EQ(emitted, 33u); // first register needs a known full-mask write
}

TEST(BpWriteCacheContract, MatchesUncachedRegistersAcrossMasksAndRawMutations) {
  BpWriteCache cache;
  struct Registers {
    std::array<uint32_t, 256> values{};
    uint32_t mask = 0x00ffffffu;
    void apply(uint32_t value) {
      const uint8_t reg = value >> 24;
      if (reg == 0xfe) { mask = value & 0x00ffffffu; return; }
      values[reg] = (values[reg] & ~mask) | (value & mask);
      mask = 0x00ffffffu;
    }
  } reference, optimized;
  uint32_t random = 0x31337;
  uint32_t previous = 0;
  const auto next = [&random] { random = random * 1664525u + 1013904223u; return random; };
  for (uint32_t i = 0; i < 20000; ++i) {
    uint32_t value = next();
    if (i % 3 == 0) value = previous;
    if (i % 11 == 0) value = 0xfe000000u | (value & 0x00ffffffu);
    if (i % 13 == 0) value = 0xc0000000u | (value & 0x00ffffffu);
    if (i % 101 == 0) {
      cache.invalidate(); // arbitrary raw list, including an unconsumed mask
      const uint32_t raw = next();
      reference.apply(raw); optimized.apply(raw);
      reference.apply(0xfe0055aau); optimized.apply(0xfe0055aau);
    }
    previous = value;
    reference.apply(value);
    if (cache.should_emit(value)) optimized.apply(value);
    ASSERT_EQ(reference.values, optimized.values) << "command " << i;
    ASSERT_EQ(reference.mask, optimized.mask) << "command " << i;
  }
}

class BpEmissionTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    fifo::set_bp_write_cache_enabled(true);
  }
  void TearDown() override {
    fifo::set_bp_write_cache_enabled(false);
    fifo::clear_buffer();
  }
};

TEST_F(BpEmissionTest, PreservesFallbackBytesAndDecodedMaterialState) {
  const auto material = [] {
    for (int repeat = 0; repeat < 5; ++repeat) {
      GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
      GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
      GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    }
  };
  material();
  const auto cached = capture_fifo();
  decode_fifo(cached);
  const auto expectedTev = gxState().tevStages;
  const auto expectedSwap = gxState().tevSwapTable;
  const auto expectedBp = gxState().bpRegCache;

  fifo::set_bp_write_cache_enabled(false);
  GXInit(nullptr, 0); fifo::clear_buffer(); reset_gx_state();
  material();
  const auto full = capture_fifo();
  ASSERT_LT(cached.size(), full.size());
  decode_fifo(full);
  EXPECT_EQ(gxState().bpRegCache, expectedBp);
  EXPECT_EQ(std::memcmp(gxState().tevStages.data(), expectedTev.data(), sizeof(expectedTev)), 0);
  EXPECT_EQ(gxState().tevSwapTable, expectedSwap);
}

TEST_F(BpEmissionTest, RecordingKeepsEveryOriginalByteAndDoesNotChangeLiveCache) {
  GX_WRITE_RAS_REG(0xc008abcd); GX_WRITE_RAS_REG(0xc008abcd);
  fifo::drain();
  alignas(32) std::array<u8, 96> list{};
  fifo::begin_display_list(list.data(), list.size());
  GX_WRITE_RAS_REG(0xc0081234); GX_WRITE_RAS_REG(0xc0081234);
  const auto recorded = fifo::end_display_list();
  EXPECT_EQ(recorded, 32u);
  const std::array<u8, 10> expected{0x61, 0xc0, 0x08, 0x12, 0x34, 0x61, 0xc0, 0x08, 0x12, 0x34};
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), list.begin()));
  GX_WRITE_RAS_REG(0xc008abcd);
  EXPECT_EQ(fifo::get_buffer_size(), 0u);

  fifo::write_stable_data(list.data(), recorded); // generic list changes live material
  GX_WRITE_RAS_REG(0xc008abcd);
  EXPECT_EQ(fifo::get_buffer_size(), recorded + 5u);
  fifo::drain();
  EXPECT_EQ(gxState().bpRegCache[0xc0], 0xc008abcdu);
}

TEST_F(BpEmissionTest, MaskConsumesEvenAnIdenticalWriteAndNextWriteRestoresFullValue) {
  GX_WRITE_RAS_REG(0xc008abcd); GX_WRITE_RAS_REG(0xc008abcd);
  GX_WRITE_RAS_REG(0xfe000000); // zero mask makes following differing data a no-op
  GX_WRITE_RAS_REG(0xc0081234);
  GX_WRITE_RAS_REG(0xc0081234); // this write must use the restored full mask
  fifo::drain();
  EXPECT_EQ(gxState().bpRegCache[0xc0], 0xc0081234u);
  EXPECT_EQ(gxState().bpRegCache[0xfe], 0x00ffffffu);
}

TEST_F(BpEmissionTest, DiscardInitAndDirectApplyInvalidatePendingIdentity) {
  GX_WRITE_RAS_REG(0xc008abcd); GX_WRITE_RAS_REG(0xc008abcd);
  fifo::clear_buffer();
  GX_WRITE_RAS_REG(0xc008abcd);
  EXPECT_EQ(fifo::get_buffer_size(), 5u);
  fifo::drain();
  GX_WRITE_RAS_REG(0xc008abcd); fifo::drain();
  GXApplyBPReg(0xc0, 0x081234);
  GX_WRITE_RAS_REG(0xc008abcd); fifo::drain();
  EXPECT_EQ(gxState().bpRegCache[0xc0], 0xc008abcdu);
  fifo::init();
  GX_WRITE_RAS_REG(0xc008abcd);
  EXPECT_EQ(fifo::get_buffer_size(), 5u);
}

TEST_F(BpEmissionTest, LittleEndianRawListInvalidatesTheLiveRegisterIdentity) {
  GX_WRITE_RAS_REG(0xc008abcd); GX_WRITE_RAS_REG(0xc008abcd);
  fifo::drain();
  const std::array<u8, 5> list{0x61, 0x34, 0x12, 0x08, 0xc0};
  GXCallDisplayListLE(list.data(), list.size());
  GX_WRITE_RAS_REG(0xc008abcd); fifo::drain();
  EXPECT_EQ(gxState().bpRegCache[0xc0], 0xc008abcdu);
}

TEST_F(BpEmissionTest, DisablingCacheImmediatelyRestoresCompleteCommandStream) {
  GX_WRITE_RAS_REG(0xc008abcd); GX_WRITE_RAS_REG(0xc008abcd); fifo::drain();
  fifo::set_bp_write_cache_enabled(false);
  GX_WRITE_RAS_REG(0xc008abcd); GX_WRITE_RAS_REG(0xc008abcd);
  EXPECT_EQ(fifo::get_buffer_size(), 10u);
  fifo::set_bp_write_cache_enabled(true);
  GX_WRITE_RAS_REG(0xc008abcd);
  EXPECT_EQ(fifo::get_buffer_size(), 15u);
}
