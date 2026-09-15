#include <gtest/gtest.h>

#include "../platforms/vita/gfx/vita_texture_decode.hpp"
#include "../platforms/vita/gfx/vita_texture_cache.hpp"

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

TEST(VitaTextureDecode, NativeI4ExpandsNibblesWithoutRgbaExpansion) {
  std::array<uint8_t,32> src{};
  src[0]=0x1e;src[1]=0x2d;
  TextureDesc d{};d.width=8;d.height=8;d.format=TextureFormat::I4;d.data=src.data();d.dataSize=src.size();
  NativeTextureFormat format=NativeTextureFormat::None;std::vector<uint8_t> out;
  ASSERT_TRUE(transcode_texture_native(d,format,out));
  EXPECT_EQ(format,NativeTextureFormat::Intensity8);ASSERT_EQ(out.size(),64u);
  EXPECT_EQ(out[0],0x11);EXPECT_EQ(out[1],0xee);EXPECT_EQ(out[2],0x22);EXPECT_EQ(out[3],0xdd);
}

TEST(VitaTextureDecode, NativeIa8SwapsGxAlphaIntensityIntoLaOrder) {
  std::array<uint8_t,32> src{};
  src[0]=0x44;src[1]=0xaa;src[2]=0x77;src[3]=0x22;
  TextureDesc d{};d.width=4;d.height=4;d.format=TextureFormat::IA8;d.data=src.data();d.dataSize=src.size();
  NativeTextureFormat format=NativeTextureFormat::None;std::vector<uint8_t> out;
  ASSERT_TRUE(transcode_texture_native(d,format,out));
  EXPECT_EQ(format,NativeTextureFormat::LuminanceAlpha8);ASSERT_EQ(out.size(),32u);
  EXPECT_EQ(out[0],0xaa);EXPECT_EQ(out[1],0x44);EXPECT_EQ(out[2],0x22);EXPECT_EQ(out[3],0x77);
}

TEST(VitaTextureDecode, NativeRgb565LinearizesTilesAndFixesEndian) {
  std::array<uint8_t,64> src{};
  // Two 4x4 GX tiles across an 8x4 texture. First texel of each tile is distinct.
  src[0]=0x12;src[1]=0x34;src[32]=0xab;src[33]=0xcd;
  TextureDesc d{};d.width=8;d.height=4;d.format=TextureFormat::RGB565;d.data=src.data();d.dataSize=src.size();
  NativeTextureFormat format=NativeTextureFormat::None;std::vector<uint8_t> out;
  ASSERT_TRUE(transcode_texture_native(d,format,out));
  EXPECT_EQ(format,NativeTextureFormat::Rgb565);ASSERT_EQ(out.size(),64u);
  EXPECT_EQ(out[0],0x34);EXPECT_EQ(out[1],0x12);
  const size_t tile1=4u*2u;EXPECT_EQ(out[tile1],0xcd);EXPECT_EQ(out[tile1+1],0xab);
}

TEST(VitaTextureCache, NonCacheableUploadsStayUniqueWithinFrameAndRetireLater) {
  constexpr size_t budget=4u*1024u*1024u;
  TextureCache cache(budget);
  const std::array<uint8_t,4> pixel{{1,2,3,4}};
  TextureDesc d{};d.width=1;d.height=1;d.format=TextureFormat::RGBA8888;
  d.data=pixel.data();d.dataSize=pixel.size();d.cacheable=false;d.sourceId=0x1234;
  const Handle first=cache.get_or_upload(d,0);
  const Handle second=cache.get_or_upload(d,0);
  ASSERT_NE(first,InvalidHandle);ASSERT_NE(second,InvalidHandle);EXPECT_NE(first,second);
  EXPECT_EQ(cache.entries(),2u);
  cache.trim(1);
  EXPECT_EQ(cache.entries(),0u);
}

} // namespace
} // namespace aurora::vita::gfx
