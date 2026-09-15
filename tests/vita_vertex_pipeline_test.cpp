#include "../platforms/vita/gfx/vita_vertex_decode.hpp"
#include "../platforms/vita/gfx/vita_vertex_pipeline.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace {

using namespace aurora::vita::gfx;

TEST(VitaVertexDecode, IndexedBigEndianS16PositionAndDirectPnMatrix) {
  // FIFO vertex: PNMTXIDX=3 (slot 1), position array index=1.
  const std::array<uint8_t,3> stream{{3,0,1}};
  // Two position records, stride 8. Record 1 is {8,-12,20} with frac=2,
  // so the decoded position must be {2,-3,5}.
  const std::array<uint8_t,16> positions{{
      0,0, 0,0, 0,0, 0,0,
      0,8, 0xff,0xf4, 0,20, 0,0,
  }};

  VertexDecodeLayout layout{};
  layout.streamStride=3;
  layout.streamLittleEndian=false;
  layout.count=2;
  layout.attributes[0]={VertexSemantic::PnMatrixIndex,VertexSource::Direct,VertexComponent::U8,1,0,0,0,{}};
  layout.attributes[1]={VertexSemantic::Position,VertexSource::Index16,VertexComponent::S16,3,2,1,0,
                        {positions.data(),positions.size(),8,false}};

  CanonicalVertex vertex{};
  ASSERT_TRUE(decode_vertex_into(stream.data(),stream.size(),0,layout,vertex));
  EXPECT_EQ(vertex.pnMatrixIndex,1u);
  EXPECT_FLOAT_EQ(vertex.position[0],2.f);
  EXPECT_FLOAT_EQ(vertex.position[1],-3.f);
  EXPECT_FLOAT_EQ(vertex.position[2],5.f);
}

TEST(VitaVertexDecode, RequiredSemanticMaskSkipsUnusedInvalidAttribute) {
  const std::array<uint8_t,3> stream{{0,0,7}};
  const std::array<uint8_t,6> positions{{0,8, 0xff,0xf4, 0,20}};

  VertexDecodeLayout layout{};
  layout.streamStride=3;
  layout.streamLittleEndian=false;
  layout.count=2;
  layout.attributes[0]={VertexSemantic::Position,VertexSource::Index16,VertexComponent::S16,3,2,0,0,
                        {positions.data(),positions.size(),6,false}};
  // Deliberately invalid array state. A pipeline that does not consume normals
  // must not resolve or decode this attribute at all.
  layout.attributes[1]={VertexSemantic::Normal,VertexSource::Index8,VertexComponent::S16,3,0,2,0,{}};

  CanonicalVertex vertex{};
  ASSERT_TRUE(decode_vertex_into(stream.data(),stream.size(),0,layout,vertex,
                                 vertex_semantic_bit(VertexSemantic::Position)));
  EXPECT_FLOAT_EQ(vertex.position[0],2.f);
  EXPECT_FLOAT_EQ(vertex.position[1],-3.f);
  EXPECT_FLOAT_EQ(vertex.position[2],5.f);

  CanonicalVertex fullVertex{};
  EXPECT_FALSE(decode_vertex_into(stream.data(),stream.size(),0,layout,fullVertex));
}

TEST(VitaVertexDecode, ExactRecordDedupPreservesTriangleCornerOrder) {
  constexpr uint16_t stride=3;
  const std::array<uint8_t,18> stream{{
      1,2,3, 4,5,6, 1,2,3,
      7,8,9, 4,5,6, 1,2,3,
  }};
  std::vector<uint8_t> compact;
  std::vector<uint16_t> remap;
  std::vector<uint32_t> table;

  ASSERT_TRUE(deduplicate_vertex_records(stream.data(),stream.size(),6,stride,compact,remap,table));
  const std::array<uint8_t,9> expectedCompact{{1,2,3,4,5,6,7,8,9}};
  const std::array<uint16_t,6> expectedRemap{{0,1,0,2,1,0}};
  ASSERT_EQ(compact.size(),expectedCompact.size());
  EXPECT_TRUE(std::equal(compact.begin(),compact.end(),expectedCompact.begin()));
  ASSERT_EQ(remap.size(),expectedRemap.size());
  EXPECT_TRUE(std::equal(remap.begin(),remap.end(),expectedRemap.begin()));
}

TEST(VitaVertexPipeline, FixedCurrentMatrixCanAddressTextureRegion) {
  CanonicalVertex vertex{};
  vertex.position[0]=1.f;
  vertex.position[1]=2.f;
  vertex.position[2]=3.f;
  vertex.position[3]=1.f;
  vertex.pnMatrixIndex=0xff;

  VertexTransformState state{};
  state.currentPnMatrix=10;
  state.postexMatrices[10].v={{1,0,0,100, 0,1,0,200, 0,0,1,300}};

  PipelineDesc pipeline{};
  VertexPipelineRequirements requirements{};
  ASSERT_TRUE(transform_vertex_for_pipeline(vertex,pipeline,state,requirements));
  EXPECT_FLOAT_EQ(vertex.position[0],101.f);
  EXPECT_FLOAT_EQ(vertex.position[1],202.f);
  EXPECT_FLOAT_EQ(vertex.position[2],303.f);
}

TEST(VitaVertexPipeline, DynamicPnMatrixRemainsLimitedToPositionPalette) {
  CanonicalVertex vertex{};
  vertex.pnMatrixIndex=10;
  VertexTransformState state{};
  PipelineDesc pipeline{};
  VertexPipelineRequirements requirements{};
  EXPECT_FALSE(transform_vertex_for_pipeline(vertex,pipeline,state,requirements));
}

TEST(VitaVertexPipeline, UnlitVertexChannelsPreserveEveryByte) {
  PipelineDesc pipeline{};
  VertexTransformState state{};
  VertexPipelineRequirements requirements{};
  requirements.colorMask=3;
  for(auto& channel:pipeline.colorChannels)channel.materialSource=ColorSource::Vertex;
  for(unsigned n=0;n<256;++n){
    CanonicalVertex vertex{};
    for(unsigned k=0;k<4;++k){
      vertex.color0[k]=static_cast<uint8_t>(n+k*61u);
      vertex.color1[k]=static_cast<uint8_t>(255u-n+k*37u);
    }
    const auto original=vertex;
    ASSERT_TRUE(transform_vertex_for_pipeline(vertex,pipeline,state,requirements));
    for(unsigned k=0;k<4;++k){
      EXPECT_EQ(vertex.color0[k],original.color0[k]);
      EXPECT_EQ(vertex.color1[k],original.color1[k]);
    }
  }
}

TEST(VitaVertexPipeline, RegisterQuantizationMatchesLroundIncludingHalfBoundaries) {
  PipelineDesc pipeline{};
  VertexTransformState state{};
  VertexPipelineRequirements requirements{};
  requirements.colorMask=1;
  const auto check=[&](float f){
    state.channelMaterial[0]={f,f,f,f};
    state.channelMaterial[2]={f,f,f,f};
    CanonicalVertex vertex{};
    ASSERT_TRUE(transform_vertex_for_pipeline(vertex,pipeline,state,requirements));
    const auto expected=static_cast<uint8_t>(std::clamp(std::lround(f*255.f),0l,255l));
    for(unsigned k=0;k<4;++k)EXPECT_EQ(vertex.color0[k],expected)<<f;
  };
  for(unsigned n=0;n<255;++n){
    const float f=(static_cast<float>(n)+0.5f)/255.f;
    check(f);check(std::nextafter(f,0.f));check(std::nextafter(f,1.f));
  }
  uint32_t random=17;
  for(unsigned n=0;n<10000;++n){
    random=random*1664525u+1013904223u;
    check(static_cast<float>(random&0xffffffu)/static_cast<float>(0xffffffu)*3.f-1.f);
  }
}

} // namespace
