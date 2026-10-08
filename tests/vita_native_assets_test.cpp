#include "gfx/vita_native_assets.hpp"
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
