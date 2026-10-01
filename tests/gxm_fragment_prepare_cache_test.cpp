#include <gtest/gtest.h>
#include "../platforms/vita/gxm/gxm_fragment_prepare_cache.hpp"
#include <array>
#include <cstring>
#include <limits>

namespace {
using aurora::vita::gxm::FragmentPrepareCache;
using aurora::vita::gxm::FragmentPrepareInputs;

TEST(FragmentPrepareCache, CopiesOwnedBytesIntoEveryFreshReservation) {
  FragmentPrepareCache cache;
  cache.configure(16);
  FragmentPrepareInputs inputs{};
  std::array<uint8_t,16> encoded{{1,2,3,4}};
  const auto expected=encoded;
  std::array<uint8_t,20> next{};
  next.fill(0xab);
  EXPECT_FALSE(cache.copy_to(inputs,next.data()+2));
  ASSERT_TRUE(cache.store(inputs,encoded.data()));
  encoded.fill(0xff); // The original GXM reservation may now be reused.
  ASSERT_TRUE(cache.copy_to(inputs,next.data()+2));
  EXPECT_EQ(std::memcmp(next.data()+2,expected.data(),expected.size()),0);
  EXPECT_EQ(next.front(),0xab);
  EXPECT_EQ(next.back(),0xab);
  next.fill(0xcd);
  ASSERT_TRUE(cache.copy_to(inputs,next.data()+2));
  EXPECT_EQ(std::memcmp(next.data()+2,expected.data(),expected.size()),0);
}

TEST(FragmentPrepareCache, RejectsEveryChangedFragmentInputWithoutWritingDestination) {
  FragmentPrepareCache cache;
  cache.configure(4);
  FragmentPrepareInputs baseline{};
  baseline.usedTextureCount=1;
  const std::array<uint8_t,4> encoded{{1,2,3,4}};
  ASSERT_TRUE(cache.store(baseline,encoded.data()));
  const auto reject=[&](const FragmentPrepareInputs& changed) {
    std::array<uint8_t,4> destination{{9,9,9,9}};
    const auto before=destination;
    EXPECT_FALSE(cache.copy_to(changed,destination.data()));
    EXPECT_EQ(destination,before);
  };
  auto changed=baseline; changed.uniforms.kcolor[0][0]+=1; reject(changed);
  changed=baseline; changed.uniforms.tevreg[0][0]+=1; reject(changed);
  changed=baseline; changed.uniforms.fogColor[0]+=1; reject(changed);
  changed=baseline; changed.uniforms.fogParams[0]+=1; reject(changed);
  changed=baseline; changed.uniforms.fogRangeK[0]+=1; reject(changed);
  changed=baseline; changed.uniforms.renderViewportWidth+=1; reject(changed);
  changed=baseline; changed.uniforms.indirectMatrices[0][0]+=1; reject(changed);
  changed=baseline; changed.uniforms.texcoordScale[0][0]+=1; reject(changed);
  changed=baseline; changed.uniforms.textureSizeBias[0][0]+=1; reject(changed);
  changed=baseline; changed.scissor.x+=1; reject(changed);
  changed=baseline; changed.scissor.y+=1; reject(changed);
  changed=baseline; changed.scissor.width+=1; reject(changed);
  changed=baseline; changed.scissor.height+=1; reject(changed);
  changed=baseline; changed.textureFlags[0]^=1; reject(changed); // EFB flip.
  changed=baseline; changed.textureFlags[0]^=4; reject(changed); // Wrap mode.
  changed=baseline; changed.textureFlags[0]^=0x40; reject(changed); // Opacity.
  changed=baseline; changed.textureFlags[0]^=0x80; reject(changed); // Copy mode.
  changed=baseline; changed.textureTransform[0][2]+=1; reject(changed);
  changed=baseline; changed.usedTextureCount=2; reject(changed);
}

TEST(FragmentPrepareCache, VertexMatricesAndUnusedTextureSlotsDoNotInvalidateFragmentBytes) {
  FragmentPrepareCache cache;
  cache.configure(4);
  FragmentPrepareInputs inputs{};
  inputs.usedTextureCount=1;
  const std::array<uint8_t,4> encoded{{1,2,3,4}};
  ASSERT_TRUE(cache.store(inputs,encoded.data()));
  inputs.uniforms.mvp[0]=123;
  inputs.uniforms.texcoordScale[1][0]=456;
  inputs.uniforms.textureSizeBias[1][0]=789;
  inputs.textureFlags[1]=0xffff;
  inputs.textureTransform[1][0]=123;
  std::array<uint8_t,4> destination{};
  ASSERT_TRUE(cache.copy_to(inputs,destination.data()));
  EXPECT_EQ(destination,encoded);
}

TEST(FragmentPrepareCache, DifferentProgramsHaveIndependentPayloadsAndInputHistory) {
  FragmentPrepareCache programA,programB;
  programA.configure(4); programB.configure(8);
  FragmentPrepareInputs inputs{};
  const std::array<uint8_t,4> encodedA{{1,2,3,4}};
  const std::array<uint8_t,8> encodedB{{9,8,7,6,5,4,3,2}};
  ASSERT_TRUE(programA.store(inputs,encodedA.data()));
  std::array<uint8_t,8> destination{};
  EXPECT_FALSE(programB.copy_to(inputs,destination.data()));
  ASSERT_TRUE(programB.store(inputs,encodedB.data()));
  ASSERT_TRUE(programB.copy_to(inputs,destination.data()));
  EXPECT_EQ(destination,encodedB);
  ASSERT_TRUE(programA.copy_to(inputs,destination.data()));
  EXPECT_EQ(std::memcmp(destination.data(),encodedA.data(),encodedA.size()),0);
}

TEST(FragmentPrepareCache, ReconfigurationAndUnsupportedSizeRestoreFallback) {
  FragmentPrepareCache cache;
  FragmentPrepareInputs inputs{};
  std::array<uint8_t,FragmentPrepareCache::MaxBytes> buffer{};
  EXPECT_FALSE(cache.store(inputs,buffer.data()));
  cache.configure(buffer.size());
  ASSERT_TRUE(cache.store(inputs,buffer.data()));
  cache.configure(8);
  EXPECT_FALSE(cache.copy_to(inputs,buffer.data()));
  cache.configure(FragmentPrepareCache::MaxBytes+1);
  EXPECT_EQ(cache.bytes(),0u);
  EXPECT_FALSE(cache.store(inputs,buffer.data()));
  EXPECT_FALSE(cache.copy_to(inputs,buffer.data()));
  cache.configure(0);
  EXPECT_EQ(cache.bytes(),0u);
}

TEST(FragmentPrepareCache, BitwiseComparisonPreservesSignedZeroAndNanPayloads) {
  FragmentPrepareCache cache;
  cache.configure(4);
  FragmentPrepareInputs inputs{};
  inputs.uniforms.fogColor[0]=0.f;
  inputs.uniforms.fogColor[1]=std::numeric_limits<float>::quiet_NaN();
  const std::array<uint8_t,4> encoded{{1,2,3,4}};
  ASSERT_TRUE(cache.store(inputs,encoded.data()));
  std::array<uint8_t,4> destination{};
  ASSERT_TRUE(cache.copy_to(inputs,destination.data()));
  inputs.uniforms.fogColor[0]=-0.f;
  EXPECT_FALSE(cache.copy_to(inputs,destination.data()));
}

TEST(FragmentPrepareCache, InvalidTextureCountAndNullBuffersRemainFallbacks) {
  FragmentPrepareCache cache;
  cache.configure(4);
  FragmentPrepareInputs inputs{};
  std::array<uint8_t,4> buffer{};
  EXPECT_FALSE(cache.store(inputs,nullptr));
  ASSERT_TRUE(cache.store(inputs,buffer.data()));
  EXPECT_FALSE(cache.copy_to(inputs,nullptr));
  inputs.usedTextureCount=aurora::vita::gfx::MaxTextures+1;
  EXPECT_FALSE(cache.store(inputs,buffer.data()));
  EXPECT_FALSE(cache.copy_to(inputs,buffer.data()));
}
} // namespace
