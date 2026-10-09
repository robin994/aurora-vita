#include "gfx/vita_native_assets.hpp"
#include "gfx/vita_fixed_vertex.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
using namespace aurora::vita::gfx;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static std::vector<uint8_t> disk;
static std::string expected;
static bool reader(const char* path,std::vector<uint8_t>& out,void*) {if(path!=expected)return false;out=disk;return true;}
int main() {
  std::vector<uint8_t> data(16384,0x5a);
  TextureDesc desc{};desc.width=desc.height=128;desc.format=TextureFormat::I8;
  desc.data=data.data();desc.dataSize=data.size();desc.immutableSource=true;desc.cacheable=false;
  auto request=native_texture_request(desc);expected=native_asset_path(request);
  CHECK(compile_native_asset(request,disk));
  configure_native_assets(reader,nullptr);
  aurora::vita::gxm::CompactTextureData native;
  CHECK(load_native_texture(desc,native,false));
  const auto reference=aurora::vita::gxm::prepare_texture_upload(desc);
  CHECK(native.pixels==reference.pixels&&native.format==reference.format&&native.stride==reference.stride);
  CHECK(native_asset_stats().textureHits==1);
  disk.back()^=1;CHECK(!load_native_texture(desc,native,false));disk.back()^=1;
  CHECK(native_asset_stats().rejected==1);
  desc.immutableSource=false;CHECK(!load_native_texture(desc,native,false));desc.immutableSource=true;
  data[0]^=1;CHECK(!load_native_texture(desc,native,false));data[0]^=1;
  for(size_t size:{size_t(0),size_t(4),size_t(27),disk.size()-1}) {auto saved=disk;disk.resize(size);CHECK(!load_native_texture(desc,native,false));disk=saved;}
  float positions[9]={0,0,0, 1,0,0, 0,1,0};
  VertexDecodeLayout layout{};layout.count=1;layout.streamStride=12;layout.streamLittleEndian=true;
  auto& a=layout.attributes[0];a.semantic=VertexSemantic::Position;a.source=VertexSource::Direct;a.component=VertexComponent::F32;a.components=3;
  compile_vertex_decode_layout(layout);
  request=native_geometry_request(reinterpret_cast<uint8_t*>(positions),sizeof positions,3,SourcePrimitive::Triangles,layout);
  expected=native_asset_path(request);CHECK(compile_native_asset(request,disk));
  PreparedDraw draw;
  CHECK(load_native_geometry(reinterpret_cast<uint8_t*>(positions),sizeof positions,3,SourcePrimitive::Triangles,layout,draw));
  CHECK(draw.vertices.size()==3&&draw.indices==std::vector<uint16_t>({0,1,2}));
  CHECK(draw.vertices[1].position[0]==1&&draw.vertices[2].position[1]==1);
  layout.attributes[0].frac=1;CHECK(!load_native_geometry(reinterpret_cast<uint8_t*>(positions),sizeof positions,3,SourcePrimitive::Triangles,layout,draw));
  CHECK(native_asset_stats().geometryHits==1);
  // Indexed BE model arrays, repeated triangles and a second mip level exercise
  // the same contracts used by the offline GLG/GLT adapter, beyond direct data.
  const uint8_t bePositions[]={0,0,0,0,0,0, 0,1,0,0,0,0, 0,0,0,1,0,0};
  std::vector<uint8_t> beIndices;
  for(unsigned triangle=0;triangle<16;++triangle)beIndices.insert(beIndices.end(),{0,1,0,2,0,0});
  layout={};layout.count=1;layout.streamStride=2;
  auto& indexed=layout.attributes[0];indexed.semantic=VertexSemantic::Position;
  indexed.source=VertexSource::Index16;indexed.component=VertexComponent::S16;indexed.components=3;
  indexed.array={bePositions,sizeof bePositions,6,false};compile_vertex_decode_layout(layout);
  request=native_geometry_request(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout);
  expected=native_asset_path(request);CHECK(compile_native_asset(request,disk));
  CHECK(load_native_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,draw));
  PipelineDesc pipeline{};pipeline.fixedVertexOnGpu=true;
  auto recipe=build_draw_recipe(pipeline);recipe.decodeSemantics=AllVertexSemantics;
  PreparedDraw referenceDraw;
  CHECK(prepare_draw_into(referenceDraw,beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,
                          layout,pipeline,{},nullptr,{},nullptr,true,&recipe));
  CHECK(draw.indices==referenceDraw.indices&&draw.vertices.size()==referenceDraw.vertices.size());
  CHECK(std::memcmp(draw.vertices.data(),referenceDraw.vertices.data(),draw.vertices.size()*sizeof(CanonicalVertex))==0);
  CHECK(draw.vertices.size()==3&&draw.indices.size()==48);
  // AVNR v2 is a separate, optional native-GXM record. Its attribute bytes
  // must equal the reference CPU pack for the exact live shader layout.
  std::vector<uint8_t> oldV1=disk,recordV2;
  CHECK(compile_native_gpu_geometry(request,recordV2));
  expected=native_gpu_geometry_path(request);
  CHECK(expected.find("native/v2/geometry/")==0);
  disk=recordV2;
  PipelineDesc gpuPipeline{};gpuPipeline.fixedVertexOnGpu=true;
  const auto gpuLayout=fixed_vertex_gpu_layout(gpuPipeline);
  NativeGpuGeometry direct;
  CHECK(load_native_gpu_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,gpuLayout,direct));
  CHECK(direct.vertexCount==referenceDraw.vertices.size());
  CHECK(direct.indices==referenceDraw.indices);
  const size_t gpuStride=gpuLayout.attributes[0].stride;
  std::vector<uint8_t> manuallyPacked(referenceDraw.vertices.size()*gpuStride,0);
  for(size_t i=0;i<referenceDraw.vertices.size();++i)
    pack_gpu_vertex_bytes(manuallyPacked.data()+i*gpuStride,referenceDraw.vertices[i],gpuLayout);
  CHECK(direct.vertices==manuallyPacked);
  CHECK(native_asset_stats().gpuGeometryHits==1);
  // A different live attribute layout must never reinterpret old packed bytes.
  gpuPipeline.fixedVertexIndexedPn=true;
  const auto different=fixed_vertex_gpu_layout(gpuPipeline);
  CHECK(!load_native_gpu_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,different,direct));
  CHECK(direct.vertices.empty()&&direct.indices.empty());
  CHECK(native_asset_stats().gpuGeometryHits==1);
  // Exact source/layout identity is part of the AVNR v2 record, not just the
  // file name. A malicious reader returning a valid file under another key is
  // rejected before any GPU handle can be created.
  beIndices[0]^=1;
  CHECK(!load_native_gpu_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,gpuLayout,direct));
  beIndices[0]^=1;
  disk.back()^=1;
  CHECK(!load_native_gpu_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,gpuLayout,direct));
  disk.back()^=1;
  CHECK(load_native_gpu_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,gpuLayout,direct));
  CHECK(direct.vertices==manuallyPacked);
  // Legacy assets are still readable after trying the independent v2 record.
  disk=oldV1;expected=native_asset_path(request);
  CHECK(load_native_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,draw));
  // Realistic mixed-attribute GPU layout (indexed PN, color, Tex0 and normal):
  // verify byte-for-byte parity even for unaligned source streams and GX
  // matrix selectors, rather than only the position-only cache profile.
  std::vector<uint8_t> attributes(3u*37u,0);
  for(unsigned i=0;i<3;++i) {
    auto* dst=attributes.data()+37u*i;
    dst[0]=uint8_t(i*3u);
    const float xyz[3]{float(i),float(i+1),-float(i)};
    const float nrm[3]{0.f,0.f,1.f};
    const float tex[2]{float(i)*0.25f,float(i)*0.125f};
    std::memcpy(dst+1,xyz,sizeof xyz);
    std::memcpy(dst+13,nrm,sizeof nrm);
    std::memcpy(dst+25,tex,sizeof tex);
    dst[33]=uint8_t(100+i);dst[34]=uint8_t(200-i);dst[35]=0xa5;dst[36]=255;
  }
  VertexDecodeLayout mixed{};mixed.count=5;mixed.streamStride=37;mixed.streamLittleEndian=true;
  auto& pn=mixed.attributes[0];pn.semantic=VertexSemantic::PnMatrixIndex;
  pn.source=VertexSource::Direct;pn.component=VertexComponent::U8;pn.components=1;pn.streamOffset=0;
  auto& pos=mixed.attributes[1];pos.semantic=VertexSemantic::Position;
  pos.source=VertexSource::Direct;pos.component=VertexComponent::F32;pos.components=3;pos.streamOffset=1;
  auto& norm=mixed.attributes[2];norm.semantic=VertexSemantic::Normal;
  norm.source=VertexSource::Direct;norm.component=VertexComponent::F32;norm.components=3;norm.streamOffset=13;
  auto& tex0=mixed.attributes[3];tex0.semantic=VertexSemantic::Tex0;
  tex0.source=VertexSource::Direct;tex0.component=VertexComponent::F32;tex0.components=2;tex0.streamOffset=25;
  auto& color=mixed.attributes[4];color.semantic=VertexSemantic::Color0;
  color.source=VertexSource::Direct;color.component=VertexComponent::RGBA8;color.components=4;color.streamOffset=33;
  compile_vertex_decode_layout(mixed);
  auto mixedReq=native_geometry_request(attributes.data(),attributes.size(),3,SourcePrimitive::Triangles,mixed);
  CHECK(compile_native_gpu_geometry(mixedReq,disk));expected=native_gpu_geometry_path(mixedReq);
  VertexLayout mixedGpu{};mixedGpu.count=5;
  mixedGpu.attributes[0]={0,4,VertexScalar::F32,false,48,0};
  mixedGpu.attributes[1]={1,4,VertexScalar::U8,true,48,16};
  mixedGpu.attributes[2]={3,3,VertexScalar::F32,false,48,20};
  mixedGpu.attributes[3]={11,3,VertexScalar::F32,false,48,32};
  mixedGpu.attributes[4]={14,1,VertexScalar::U8,false,48,44};
  CHECK(load_native_gpu_geometry(attributes.data(),attributes.size(),3,SourcePrimitive::Triangles,mixed,mixedGpu,direct));
  CHECK(direct.vertexCount==3&&direct.indices==std::vector<uint16_t>({0,1,2}));
  PreparedDraw mixedReference;PipelineDesc staticPipeline{};staticPipeline.fixedVertexOnGpu=true;
  auto fullRecipe=build_draw_recipe(staticPipeline);fullRecipe.decodeSemantics=AllVertexSemantics;
  CHECK(prepare_draw_into(mixedReference,attributes.data(),attributes.size(),3,SourcePrimitive::Triangles,
                          mixed,staticPipeline,{},nullptr,{},nullptr,true,&fullRecipe));
  manuallyPacked.assign(3*48,0);
  for(unsigned i=0;i<3;++i)
    pack_gpu_vertex_bytes(manuallyPacked.data()+i*48,mixedReference.vertices[i],mixedGpu);
  CHECK(direct.vertices==manuallyPacked);
  // Malformed or truncated AVNR v2 records must not publish an incomplete draw.
  for(size_t n:{size_t(0),size_t(2),size_t(27),disk.size()-1}) {
    const auto saved=disk;disk.resize(n);
    CHECK(!load_native_gpu_geometry(attributes.data(),attributes.size(),3,SourcePrimitive::Triangles,mixed,mixedGpu,direct));
    CHECK(direct.vertices.empty());disk=saved;
  }
  data.resize(16384+4096);desc.data=data.data();desc.dataSize=data.size();desc.mipCount=2;
  request=native_texture_request(desc);expected=native_asset_path(request);CHECK(compile_native_asset(request,disk));
  CHECK(load_native_texture(desc,native,false));
  const auto mipReference=aurora::vita::gxm::prepare_texture_upload(desc);
  CHECK(native.mipCount==2&&native.pixels==mipReference.pixels);
  desc.format=TextureFormat::RGBA8888;desc.mipCount=1;data.resize(128*128*4);
  desc.data=data.data();desc.dataSize=data.size();CHECK(!compile_native_asset(native_texture_request(desc),disk));
  request=native_texture_request(desc);expected=native_asset_path(request);
  CHECK(compile_native_asset(request,disk,true));CHECK(load_native_texture(desc,native,false));
  const auto rgbaReference=aurora::vita::gxm::prepare_texture_upload(desc);
  CHECK(native.format==NativeTextureFormat::Rgba8&&native.pixels==rgbaReference.pixels);
  CHECK(native.mipCount==rgbaReference.mipCount&&native.stride==rgbaReference.stride);
  std::vector<uint8_t> blob;
  CHECK(read_native_blob(expected.c_str(),blob,disk.size()));CHECK(blob==disk);
  CHECK(!read_native_blob(expected.c_str(),blob,disk.size()-1));CHECK(blob.empty());
  configure_native_assets(nullptr,nullptr);
  std::puts("native asset exact source, payload checksum, bounds, immutable eligibility and layout invalidation passed");
}
