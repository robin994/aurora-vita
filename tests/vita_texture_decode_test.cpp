#include <gtest/gtest.h>

#include "../platforms/vita/gfx/vita_texture_decode.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace aurora::vita::gfx {
namespace {

TEST(VitaTextureDecode, Dxt1SizeUsesFourByFourBlocks) {
  EXPECT_EQ(dxt1_texture_size(1,1),8u);
  EXPECT_EQ(dxt1_texture_size(4,4),8u);
  EXPECT_EQ(dxt1_texture_size(8,4),16u);
  EXPECT_EQ(dxt1_texture_size(8,8),32u);
  EXPECT_EQ(dxt1_texture_size(16,8),64u);
}

TEST(VitaTextureDecode, CmprToDxt1FixesEndianAndSelectorOrder) {
  std::array<uint8_t,32> cmpr{};
  cmpr[0]=0x12;cmpr[1]=0x34;cmpr[2]=0x56;cmpr[3]=0x78;
  cmpr[4]=0x1b;cmpr[5]=0xe4;cmpr[6]=0x6c;cmpr[7]=0x93;
  TextureDesc d{};d.width=8;d.height=8;d.format=TextureFormat::CMPR;d.data=cmpr.data();d.dataSize=cmpr.size();
  std::vector<uint8_t> out;
  ASSERT_TRUE(transcode_cmpr_to_dxt1(d,out));
  ASSERT_EQ(out.size(),32u);
  EXPECT_EQ(out[0],0x34);EXPECT_EQ(out[1],0x12);
  EXPECT_EQ(out[2],0x78);EXPECT_EQ(out[3],0x56);
  EXPECT_EQ(out[4],0xe4);EXPECT_EQ(out[5],0x1b);
  EXPECT_EQ(out[6],0x39);EXPECT_EQ(out[7],0xc6);
}

TEST(VitaTextureDecode, CmprToDxt1LinearizesEightByEightMacroTiles) {
  std::array<uint8_t,64> cmpr{};
  // Two adjacent GX 8x8 macro tiles, each containing TL/TR/BL/BR sub-blocks.
  for(unsigned block=0;block<8;block++){
    auto* p=cmpr.data()+block*8u;
    p[0]=0;p[1]=static_cast<uint8_t>(block+1);p[2]=0;p[3]=0;
  }
  TextureDesc d{};d.width=16;d.height=8;d.format=TextureFormat::CMPR;d.data=cmpr.data();d.dataSize=cmpr.size();
  std::vector<uint8_t> out;
  ASSERT_TRUE(transcode_cmpr_to_dxt1(d,out));
  ASSERT_EQ(out.size(),64u);
  // DXT1 block rows are TL/TR from macro 0, then TL/TR from macro 1,
  // followed by both bottom pairs.
  const std::array<uint8_t,8> expected{{1,2,5,6,3,4,7,8}};
  for(size_t i=0;i<expected.size();i++)EXPECT_EQ(out[i*8],expected[i])<<"block "<<i;
}

} // namespace
} // namespace aurora::vita::gfx
