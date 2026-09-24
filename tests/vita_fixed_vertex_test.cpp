#include "../platforms/vita/gfx/vita_fixed_vertex.hpp"
#include "../platforms/vita/gfx/vita_static_geometry.hpp"
#include "../platforms/vita/gfx/vita_shader_gen.hpp"
#include "../platforms/vita/gfx/vita_pipeline_key.hpp"
#include "../platforms/vita/gfx/vita_program_binary_cache.hpp"
#include "../platforms/vita/gxm/gxm_shader_gen.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstring>

namespace {
using namespace aurora::vita::gfx;

TEST(VitaGeometryBytes, ExactEqualityForUnalignedSpansAndEveryChangedByte) {
  std::array<uint8_t,544> left{},right{};
  const std::array<size_t,16> lengths{{0,1,2,3,7,15,16,17,31,63,64,65,127,128,255,513}};
  for(size_t a=0;a<16;++a)for(size_t b=0;b<16;++b)for(const size_t n:lengths) {
    for(size_t i=0;i<n;++i)left[a+i]=right[b+i]=static_cast<uint8_t>((i*173u+37u)&255u);
    ASSERT_TRUE(byte_spans_equal(left.data()+a,right.data()+b,n));
    for(size_t i=0;i<n;++i) {
      right[b+i]^=0x80u;
      ASSERT_FALSE(byte_spans_equal(left.data()+a,right.data()+b,n))<<a<<','<<b<<','<<n<<','<<i;
      right[b+i]^=0x80u;
    }
  }
  EXPECT_TRUE(byte_spans_equal(nullptr,nullptr,0));
  EXPECT_FALSE(byte_spans_equal(nullptr,right.data(),1));
  EXPECT_TRUE(byte_spans_equal(left.data(),left.data(),left.size()));
}

TEST(VitaGeometryBytes, IgnoresBytesOutsideRequestedSpan) {
  std::array<uint8_t,66> left{},right{};
  right.front()=1;right.back()=1;
  EXPECT_TRUE(byte_spans_equal(left.data()+1,right.data()+1,64));
  EXPECT_FALSE(byte_spans_equal(left.data(),right.data(),65));
  EXPECT_FALSE(byte_spans_equal(left.data()+1,right.data()+1,65));
}

struct IndexedFixture {
  std::array<float,9> positions{{1,2,3, 4,5,6, 7,8,9}};
  std::array<uint8_t,60> raw{};
  VertexDecodeLayout layout{};
  PipelineDesc pipeline{};
  VertexTransformState state{};
  IndexedFixture() {
    for(unsigned i=0;i<raw.size();++i)raw[i]=static_cast<uint8_t>(i%3);
    layout.count=1;layout.streamStride=1;
    layout.attributes[0]={VertexSemantic::Position,VertexSource::Index8,VertexComponent::F32,3,0,0,0,
      {reinterpret_cast<const uint8_t*>(positions.data()),sizeof(positions),12,true}};
    pipeline.fixedVertexOnGpu=true;
  }
};

TEST(VitaFixedVertex, RejectsUnsupportedHostGpuFeaturesButIgnoresUnusedSelectors) {
  IndexedFixture f;
  EXPECT_TRUE(supports_fixed_vertex_gpu(f.pipeline,f.layout,f.state));
  f.pipeline.tev.stages[0].color.d=TevColorArg::RasColor;
  f.pipeline.colorChannels[0].lightingEnabled=true;
  EXPECT_FALSE(supports_fixed_vertex_gpu(f.pipeline,f.layout,f.state));
  f.pipeline.colorChannels[0].lightingEnabled=false;
  f.layout.attributes[1]={VertexSemantic::PnMatrixIndex,VertexSource::Direct,VertexComponent::U8,1,0,0,0,{}};
  f.layout.count=2;
  EXPECT_FALSE(supports_fixed_vertex_gpu(f.pipeline,f.layout,f.state));
  f.layout.attributes[1].semantic=VertexSemantic::TexMatrixIndex0;
  // Merely carrying an unused selector is harmless. It becomes a GPU input only
  // when a live texgen requests matrixFromVertex.
  EXPECT_TRUE(supports_fixed_vertex_gpu(f.pipeline,f.layout,f.state));
  f.pipeline.texgenCount=1;
  f.pipeline.tev.stages[0].texture=0;
  f.pipeline.tev.stages[0].texCoord=0;
  f.pipeline.tev.stages[0].color.a=TevColorArg::TexColor;
  f.pipeline.texgens[0].matrixFromVertex=true;
  EXPECT_FALSE(supports_fixed_vertex_gpu(f.pipeline,f.layout,f.state));
}

TEST(VitaFixedVertex, RejectsEmbossAndInvalidMatrixIndices) {
  IndexedFixture f;
  auto& p=f.pipeline;
  p.tev.stages[0].texture=0;p.tev.stages[0].texCoord=0;
  p.tev.stages[0].color.a=TevColorArg::TexColor;p.texgenCount=1;
  p.texgens[0].type=TexGenType::Bump0;
  EXPECT_FALSE(supports_fixed_vertex_gpu(p,f.layout,f.state));
  p.texgens[0].type=TexGenType::Matrix2x4;
  p.texgens[0].matrix=10;
  EXPECT_FALSE(supports_fixed_vertex_gpu(p,f.layout,f.state));
  p.texgens[0].matrix=0;
  f.state.currentPnMatrix=19;
  EXPECT_TRUE(supports_fixed_vertex_gpu(p,f.layout,f.state));
  f.state.currentPnMatrix=20;
  EXPECT_FALSE(supports_fixed_vertex_gpu(p,f.layout,f.state));
}

TEST(VitaFixedVertex, InputsFollowRawTexgenSourcesInsteadOfOutputCoordinates) {
  IndexedFixture f;
  auto& p=f.pipeline;p.texgenCount=2;
  p.tev.stageCount=2;
  for(unsigned i=0;i<2;++i){p.tev.stages[i].texture=i;p.tev.stages[i].texCoord=i;p.tev.stages[i].color.a=TevColorArg::TexColor;}
  p.texgens[0].source=TexGenSource::Tex5;
  p.texgens[1].source=TexGenSource::Normal;
  const auto inputs=fixed_vertex_gpu_inputs(p);
  EXPECT_NE(inputs&vertex_semantic_bit(VertexSemantic::Tex5),0u);
  EXPECT_NE(inputs&vertex_semantic_bit(VertexSemantic::Normal),0u);
  EXPECT_EQ(inputs&vertex_semantic_bit(VertexSemantic::Tex0),0u);
  const auto shader=build_tev_glsl(p);
  EXPECT_NE(shader.vertex.find("attribute vec3 a_tex5"),std::string::npos);
  EXPECT_NE(shader.vertex.find("attribute vec3 a_normal"),std::string::npos);
  EXPECT_EQ(shader.vertex.find("attribute vec3 a_tex0"),std::string::npos);
  EXPECT_NE(shader.vertex.find("varying vec3 v_tex0"),std::string::npos);
  EXPECT_NE(shader.vertex.find("gl_Position=u_mvp*vec4(mv,a_position.w)"),std::string::npos);
  const auto gpu=fixed_vertex_gpu_layout(p);
  EXPECT_EQ(gpu.count,3u);
  EXPECT_EQ(gpu.attributes[0].stride,40u);
  EXPECT_EQ(gpu.attributes[1].location,8u);
  EXPECT_EQ(gpu.attributes[2].location,11u);
}

TEST(VitaFixedVertex, NonColorSrtgRetainsTheEstablishedCpuPath) {
  IndexedFixture f;
  auto& p=f.pipeline;p.texgenCount=1;
  p.tev.stages[0].texture=0;p.tev.stages[0].texCoord=0;
  p.tev.stages[0].color.a=TevColorArg::TexColor;
  p.texgens[0].type=TexGenType::SRTG;
  p.texgens[0].source=TexGenSource::Position;
  EXPECT_FALSE(supports_fixed_vertex_gpu(p,f.layout,f.state));
  p.texgens[0].source=TexGenSource::Color1;
  EXPECT_TRUE(supports_fixed_vertex_gpu(p,f.layout,f.state));
}

TEST(VitaFixedVertex, MatrixSnapshotsMatchCpuTransformAndRemainImmutable) {
  IndexedFixture f;
  f.pipeline.texgenCount=1;
  auto& stage=f.pipeline.tev.stages[0];stage.texture=0;stage.texCoord=0;stage.color.a=TevColorArg::TexColor;
  auto& tg=f.pipeline.texgens[0];tg.type=TexGenType::Matrix3x4;tg.source=TexGenSource::Position;tg.matrix=3;tg.postMatrix=4;
  f.state.postexMatrices[13].v={2,0,1,3, 0,3,1,4, 1,0,2,5};
  f.state.postMatrices[4].v={1,2,0,1, 2,0,1,2, 0,1,3,3};
  for(unsigned pn=0;pn<20;++pn){
    f.state.currentPnMatrix=static_cast<uint8_t>(pn);
    const auto old=f.state.postexMatrices[pn];
    f.state.postexMatrices[pn].v={1,2,3,4, -2,1,0,6, 0,1,2,8};
    const auto uniform=fixed_vertex_uniforms(f.pipeline,f.state);
    CanonicalVertex v{};v.position[0]=1.25f;v.position[1]=-3.5f;v.position[2]=2.75f;
    const auto raw=v;
    ASSERT_TRUE(transform_vertex_for_pipeline(v,f.pipeline,f.state,vertex_pipeline_requirements(f.pipeline)));
    const auto dot=[](const std::array<float,12>&m,const std::array<float,4>&p){
      std::array<float,3> r{};for(unsigned k=0;k<3;++k)r[k]=p[0]*m[k*4]+p[1]*m[k*4+1]+p[2]*m[k*4+2]+p[3]*m[k*4+3];return r;
    };
    const std::array<float,4> pos{{raw.position[0],raw.position[1],raw.position[2],1}};
    const auto mv=dot(uniform.position,pos);
    auto tex=dot(uniform.texture[0],pos);
    tex=dot(uniform.post[0],{tex[0],tex[1],tex[2],1});
    for(unsigned k=0;k<3;++k){EXPECT_FLOAT_EQ(v.position[k],mv[k]);EXPECT_FLOAT_EQ(v.texcoord[0][k],tex[k]);}
    f.state.postexMatrices[pn].v[0]+=100.f;
    EXPECT_NE(uniform.position[0],f.state.postexMatrices[pn].v[0]);
    f.state.postexMatrices[pn]=old;
  }
}

TEST(VitaFixedVertex, CacheReusesRawGeometryAcrossCameraChanges) {
  IndexedFixture f;Renderer renderer;ASSERT_TRUE(renderer.initialize());
  StaticGeometryCache cache(renderer,1024*1024);
  const auto* first=cache.get(f.raw.data(),f.raw.size(),f.raw.size(),SourcePrimitive::Triangles,f.layout,f.pipeline,f.state,nullptr);
  ASSERT_NE(first,nullptr);EXPECT_EQ(first->vertexCount,3u);EXPECT_EQ(first->indexCount,60u);
  const auto buffer=first->vertices.buffer;
  f.state.postexMatrices[0].v[3]=42.f;
  const auto* second=cache.get(f.raw.data(),f.raw.size(),f.raw.size(),SourcePrimitive::Triangles,f.layout,f.pipeline,f.state,nullptr);
  ASSERT_NE(second,nullptr);EXPECT_EQ(second->vertices.buffer,buffer);EXPECT_EQ(cache.hits(),1u);
}

TEST(VitaFixedVertex, PersistentCacheAcceptsExplicitTriangleIndices) {
  IndexedFixture f;Renderer renderer;ASSERT_TRUE(renderer.initialize());
  StaticGeometryCache cache(renderer,1024*1024);
  const std::array<uint16_t,3> indices{{2,1,0}};
  auto* first=cache.get(f.raw.data(),3,3,SourcePrimitive::Triangles,f.layout,f.pipeline,
                        f.state,nullptr,nullptr,indices.data(),indices.size());
  ASSERT_NE(first,nullptr);
  EXPECT_EQ(first->vertexCount,3u);
  EXPECT_EQ(first->indexCount,3u);
  const auto vb=first->vertices.buffer;
  auto* second=cache.get(f.raw.data(),3,3,SourcePrimitive::Triangles,f.layout,f.pipeline,
                         f.state,nullptr,nullptr,indices.data(),indices.size());
  ASSERT_NE(second,nullptr);
  EXPECT_EQ(second->vertices.buffer,vb);
  EXPECT_EQ(cache.hits(),1u);
}

TEST(VitaFixedVertex, PersistentCacheExpandsStablePointsAndLinesOnce) {
  IndexedFixture f;Renderer renderer;ASSERT_TRUE(renderer.initialize());
  StaticGeometryCache cache(renderer,1024*1024);
  f.pipeline.fixedPointSprite=true;
  auto* point=cache.get(f.raw.data(),3,3,SourcePrimitive::Points,f.layout,f.pipeline,f.state,nullptr);
  ASSERT_NE(point,nullptr);
  EXPECT_EQ(point->vertexCount,12u);
  EXPECT_EQ(point->indexCount,18u);

  f.pipeline.fixedPointSprite=false;
  f.pipeline.fixedLineSprite=true;
  auto* line=cache.get(f.raw.data(),2,2,SourcePrimitive::Lines,f.layout,f.pipeline,f.state,nullptr);
  ASSERT_NE(line,nullptr);
  EXPECT_EQ(line->vertexCount,4u);
  EXPECT_EQ(line->indexCount,6u);
}

TEST(VitaFixedVertex, PackedMatrixSelectorLayoutUsesOneFloat3Slot) {
  IndexedFixture f;
  f.pipeline.fixedVertexTexMtxMask=0x81;
  const auto layout=fixed_vertex_gpu_layout(f.pipeline);
  ASSERT_GT(layout.count,1u);
  const auto& selector=layout.attributes[layout.count-1];
  EXPECT_EQ(selector.location,14u);
  EXPECT_EQ(selector.components,3u);
  EXPECT_EQ(selector.scalar,VertexScalar::F32);
}

TEST(VitaFixedVertex, NativeCgConsumesPackedTextureMatrixSelectors) {
  PipelineDesc p{};
  p.fixedVertexOnGpu=true;
  p.fixedVertexTexMtxMask=1;
  p.texgenCount=1;
  p.tev.stages[0].texture=0;
  p.tev.stages[0].texCoord=0;
  p.tev.stages[0].color.a=TevColorArg::TexColor;
  p.texgens[0].source=TexGenSource::Position;
  p.texgens[0].matrixFromVertex=true;
  p.layout=fixed_vertex_gpu_layout(p);
  const auto shader=aurora::vita::gxm::build_tev_cg(p);
  ASSERT_TRUE(shader.ok())<<shader.error;
  EXPECT_NE(shader.vertex.find("a_matrix_sel"),std::string::npos);
  EXPECT_NE(shader.vertex.find("gx_tm0"),std::string::npos);
  EXPECT_NE(shader.vertex.find("u_gx_texture_palette"),std::string::npos);
}

TEST(VitaFixedVertex, NativeCgKeepsBumpBasisAndSelectedLightOnGpu) {
  PipelineDesc p{};
  p.fixedVertexOnGpu=true;
  p.texgenCount=2;
  p.tev.stages[0].texture=0;
  p.tev.stages[0].texCoord=1;
  p.tev.stages[0].color.a=TevColorArg::TexColor;
  p.texgens[0].source=TexGenSource::Tex0;
  p.texgens[1].type=TexGenType::Bump3;
  p.texgens[1].embossSource=0;
  const auto inputs=fixed_vertex_gpu_inputs(p);
  EXPECT_NE(inputs&vertex_semantic_bit(VertexSemantic::Binormal),0u);
  EXPECT_NE(inputs&vertex_semantic_bit(VertexSemantic::Tangent),0u);
  p.layout=fixed_vertex_gpu_layout(p);
  const auto shader=aurora::vita::gxm::build_tev_cg(p);
  ASSERT_TRUE(shader.ok())<<shader.error;
  EXPECT_NE(shader.vertex.find("a_binormal"),std::string::npos);
  EXPECT_NE(shader.vertex.find("a_tangent"),std::string::npos);
  EXPECT_NE(shader.vertex.find("u_gx_light[15]"),std::string::npos);
}

TEST(VitaFixedVertex, NativeCgExpandsCachedPointsAndLinesInVertexShader) {
  PipelineDesc point{};
  point.fixedVertexOnGpu=true;
  point.fixedPointSprite=true;
  point.primitive=Primitive::Triangles;
  point.layout=fixed_vertex_gpu_layout(point);
  auto shader=aurora::vita::gxm::build_tev_cg(point);
  ASSERT_TRUE(shader.ok())<<shader.error;
  EXPECT_NE(shader.vertex.find("u_gx_primitive_expand"),std::string::npos);
  EXPECT_NE(shader.vertex.find("gx_corner"),std::string::npos);

  PipelineDesc line=point;
  line.fixedPointSprite=false;
  line.fixedLineSprite=true;
  line.layout=fixed_vertex_gpu_layout(line);
  shader=aurora::vita::gxm::build_tev_cg(line);
  ASSERT_TRUE(shader.ok())<<shader.error;
  EXPECT_NE(shader.vertex.find("a_line_other"),std::string::npos);
  EXPECT_NE(shader.vertex.find("gx_other"),std::string::npos);
}

TEST(VitaFixedVertex, ChangedArrayDataCannotReuseAnInFlightImmutableBuffer) {
  IndexedFixture f;Renderer renderer;ASSERT_TRUE(renderer.initialize());
  StaticGeometryCache cache(renderer,1024*1024);
  ASSERT_NE(cache.get(f.raw.data(),f.raw.size(),f.raw.size(),SourcePrimitive::Triangles,f.layout,f.pipeline,f.state,nullptr),nullptr);
  f.positions[4]+=1.f;
  EXPECT_EQ(cache.get(f.raw.data(),f.raw.size(),f.raw.size(),SourcePrimitive::Triangles,f.layout,f.pipeline,f.state,nullptr),nullptr);
  f.positions[4]-=1.f;
  EXPECT_EQ(cache.get(f.raw.data(),f.raw.size(),f.raw.size(),SourcePrimitive::Triangles,f.layout,f.pipeline,f.state,nullptr),nullptr);
  EXPECT_EQ(cache.hits(),0u);
}

TEST(VitaFixedVertex, SnapshotBoundsAndMemoryBudgetAreEnforced) {
  IndexedFixture f;
  std::vector<StaticGeometryCache::Snapshot> snapshots;
  ASSERT_TRUE(StaticGeometryCache::snapshot_sources(f.raw.data(),f.raw.size(),f.raw.size(),f.layout,
    fixed_vertex_gpu_inputs(f.pipeline),snapshots));
  ASSERT_EQ(snapshots.size(),1u);EXPECT_EQ(snapshots[0].bytes.size(),sizeof(f.positions));
  f.raw.back()=3;
  EXPECT_FALSE(StaticGeometryCache::snapshot_sources(f.raw.data(),f.raw.size(),f.raw.size(),f.layout,
    fixed_vertex_gpu_inputs(f.pipeline),snapshots));
  f.raw.back()=2;
  Renderer renderer;ASSERT_TRUE(renderer.initialize());StaticGeometryCache cache(renderer,1);
  EXPECT_EQ(cache.get(f.raw.data(),f.raw.size(),f.raw.size(),SourcePrimitive::Triangles,f.layout,f.pipeline,f.state,nullptr),nullptr);
  EXPECT_EQ(cache.bytes(),0u);EXPECT_EQ(cache.size(),0u);
}

TEST(VitaFixedVertex, PipelineKeySeparatesCpuAndGpuVertexConventions) {
  PipelineDesc p{};const auto cpu=pipeline_key(p);p.fixedVertexOnGpu=true;
  EXPECT_NE(cpu,pipeline_key(p));
}

TEST(VitaProgramCache, KeysCoverBothShaderStagesAndTheirBoundary) {
  EXPECT_NE(program_source_hash("vertex-a","fragment"),program_source_hash("vertex-b","fragment"));
  EXPECT_NE(program_source_hash("ab","c"),program_source_hash("a","bc"));
}

TEST(VitaProgramCache, RejectsTruncationCorruptionAndWrongSource) {
  constexpr size_t attributes=132;
  std::array<uint8_t,attributes+4+16+4+16> binary{};
  const uint32_t size=16;
  std::memcpy(binary.data()+attributes,&size,4);
  std::memcpy(binary.data()+attributes+4+size,&size,4);
  ProgramCacheHeader header{};header.sourceHash=42;header.length=binary.size();
  header.binaryHash=program_cache_hash(binary.data(),binary.size());
  EXPECT_TRUE(valid_program_cache(header,binary.data(),binary.size(),42,attributes));
  EXPECT_FALSE(valid_program_cache(header,binary.data(),binary.size()-1,42,attributes));
  EXPECT_FALSE(valid_program_cache(header,binary.data(),binary.size(),43,attributes));
  binary.back()=1;
  EXPECT_FALSE(valid_program_cache(header,binary.data(),binary.size(),42,attributes));
}

TEST(VitaProgramCache, ChecksPrivateSerializerLengthsBeforeDeserializing) {
  constexpr size_t attributes=132;
  std::array<uint8_t,attributes+4+16+4+16> binary{};
  uint32_t size=16;
  std::memcpy(binary.data()+attributes,&size,4);
  std::memcpy(binary.data()+attributes+4+size,&size,4);
  ProgramCacheHeader header{};header.sourceHash=42;header.length=binary.size();
  uint32_t bad=0xffffffff;
  std::memcpy(binary.data()+attributes,&bad,4);
  header.binaryHash=program_cache_hash(binary.data(),binary.size());
  EXPECT_FALSE(valid_program_cache(header,binary.data(),binary.size(),42,attributes));
  std::memcpy(binary.data()+attributes,&size,4);
  std::memcpy(binary.data()+attributes+4,&bad,4);
  header.binaryHash=program_cache_hash(binary.data(),binary.size());
  EXPECT_FALSE(valid_program_cache(header,binary.data(),binary.size(),42,attributes));
}
} // namespace
