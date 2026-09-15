#include "../platforms/vita/gfx/vita_vertex_decode.hpp"
#include "../platforms/vita/gfx/vita_vertex_pipeline.hpp"

#include <gtest/gtest.h>

#include <array>

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

TEST(VitaVertexPipeline, ComposedModelProjectionMatchesCpuPositionTransform) {
  Matrix3x4 model{};
  model.v={{2.f,0.f,0.f,5.f, 0.f,3.f,0.f,-7.f, 0.f,0.f,4.f,11.f}};
  const std::array<float,16> projection{{
      1.5f,0.f,0.f,0.f,
      0.f,2.f,0.f,0.f,
      0.f,0.f,0.5f,0.f,
      0.25f,-0.5f,1.f,1.f}};
  const auto combined=compose_model_projection(projection,model);
  const std::array<float,4> p{{1.f,2.f,3.f,1.f}};
  const auto apply=[](const std::array<float,16>&m,const std::array<float,4>&v){
    std::array<float,4> o{};
    for(unsigned row=0;row<4;++row)for(unsigned k=0;k<4;++k)o[row]+=m[k*4+row]*v[k];
    return o;
  };
  const std::array<float,4> modelPos{{7.f,-1.f,23.f,1.f}};
  const auto expected=apply(projection,modelPos);
  const auto actual=apply(combined,p);
  for(unsigned i=0;i<4;++i)EXPECT_FLOAT_EQ(actual[i],expected[i]);
}

} // namespace
