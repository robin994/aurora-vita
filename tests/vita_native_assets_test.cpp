#include "gfx/vita_native_assets.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <new>
namespace {
std::atomic<size_t> denySize1{0},denySize2{0},deniedAllocations{0};
std::atomic<bool> failReader{false};
std::atomic<const uint8_t*> readerBuffer{nullptr};
}
// Reject exactly the redundant payload/pixel allocations seen in the Vita
// dump. The larger reader-owned AVNR buffer remains available for transfer.
void* operator new(size_t bytes) {
  if(bytes&&(bytes==denySize1.load()||bytes==denySize2.load())) {
    ++deniedAllocations;throw std::bad_alloc();
  }
  if(auto* p=std::malloc(bytes?bytes:1))return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
using namespace aurora::vita::gfx;
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static std::vector<uint8_t> disk;
static std::string expected;
static bool reader(const char* path,std::vector<uint8_t>& out,void*) {
  if(path!=expected)return false;
  if(failReader)throw std::bad_alloc();
  out=disk;readerBuffer=out.data();return true;
}
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
  // The same offline object-space draw must follow changed camera/pose state
  // through the production transform worker without changing its topology.
  pipeline.fixedVertexOnGpu=false;recipe=build_draw_recipe(pipeline);
  VertexTransformState animated{};animated.postexMatrices[0].v[3]=2.f;
  PreparedDraw converted,ordinary;
  CHECK(prepare_native_geometry(converted,beIndices.data(),beIndices.size(),48,
      SourcePrimitive::Triangles,layout,pipeline,animated,recipe));
  CHECK(prepare_draw_into(ordinary,beIndices.data(),beIndices.size(),48,
      SourcePrimitive::Triangles,layout,pipeline,animated,nullptr,{},nullptr,true,&recipe));
  CHECK(converted.indices==ordinary.indices&&converted.vertices.size()==ordinary.vertices.size());
  CHECK(std::memcmp(converted.vertices.data(),ordinary.vertices.data(),converted.vertices.size()*sizeof(CanonicalVertex))==0);
  animated.postexMatrices[0].v[3]=-3.f;
  CHECK(prepare_native_geometry(converted,beIndices.data(),beIndices.size(),48,
      SourcePrimitive::Triangles,layout,pipeline,animated,recipe));
  CHECK(prepare_draw_into(ordinary,beIndices.data(),beIndices.size(),48,
      SourcePrimitive::Triangles,layout,pipeline,animated,nullptr,{},nullptr,true,&recipe));
  CHECK(std::memcmp(converted.vertices.data(),ordinary.vertices.data(),converted.vertices.size()*sizeof(CanonicalVertex))==0);
  data.resize(16384+4096);desc.data=data.data();desc.dataSize=data.size();desc.mipCount=2;
  request=native_texture_request(desc);expected=native_asset_path(request);CHECK(compile_native_asset(request,disk));
  CHECK(load_native_texture(desc,native,false));
  const auto mipReference=aurora::vita::gxm::prepare_texture_upload(desc);
  CHECK(native.mipCount==2&&native.pixels==mipReference.pixels);
  desc.format=TextureFormat::RGBA8888;desc.mipCount=1;data.resize(128*128*4);
  for(size_t i=0;i<data.size();++i)data[i]=uint8_t(i*37u);
  desc.data=data.data();desc.dataSize=data.size();CHECK(!compile_native_asset(native_texture_request(desc),disk,true,false));
  request=native_texture_request(desc);expected=native_asset_path(request);
  CHECK(compile_native_asset(request,disk,true));CHECK(load_native_texture(desc,native,false));
  const auto rgbaReference=aurora::vita::gxm::prepare_texture_upload(desc);
  CHECK(native.format==NativeTextureFormat::Rgba8&&native.pixels==rgbaReference.pixels);
  CHECK(native.mipCount==rgbaReference.mipCount&&native.stride==rgbaReference.stride);
  std::vector<uint8_t> blob;
  CHECK(read_native_blob(expected.c_str(),blob,disk.size()));CHECK(blob==disk);
  CHECK(!read_native_blob(expected.c_str(),blob,disk.size()-1));CHECK(blob.empty());
  // A producer configured for the baseline renderer must not leave BC1
  // records which its consumer rejects and decodes again on the console.
  desc.width=desc.height=8;desc.format=TextureFormat::CMPR;data.assign(32,0);
  for(unsigned block=0;block<4;++block){data[block*8]=255;data[block*8+1]=255;}
  desc.data=data.data();desc.dataSize=data.size();
  request=native_texture_request(desc);expected=native_asset_path(request);
  CHECK(compile_native_asset(request,disk,false));CHECK(load_native_texture(desc,native,false));
  const auto cmprReference=aurora::vita::gxm::prepare_texture_upload(desc,true,false);
  CHECK(native.format!=NativeTextureFormat::Bc1&&native.pixels==cmprReference.pixels);
  // The GX consumer can update asset counts while the game samples diagnostics.
  // Reader configuration and source data stay fixed for the worker lifetime.
  const auto before=native_asset_stats().textureHits;
  std::atomic<unsigned> completed{0},successful{0};std::thread workers[4];
  for(auto& worker:workers)worker=std::thread([&] {
    for(unsigned n=0;n<16;++n) {
      aurora::vita::gxm::CompactTextureData local;
      if(load_native_texture(desc,local,false)&&local.pixels==cmprReference.pixels)++successful;
    }
    ++completed;
  });
  while(completed.load()!=4){(void)native_asset_stats();std::this_thread::yield();}
  for(auto& worker:workers)worker.join();
  CHECK(successful==64&&native_asset_stats().textureHits==before+64);
  // A 1024-square CMPR source expands to the 4 MiB RGBA payload implicated
  // in race loading. Deny both intermediate allocations while allowing the
  // complete record returned by the reader: it must be moved, not copied.
  desc.width=desc.height=1024;desc.mipCount=1;desc.format=TextureFormat::CMPR;
  data.assign(encoded_texture_size(desc.width,desc.height,desc.format),0);
  desc.data=data.data();desc.dataSize=data.size();
  request=native_texture_request(desc);CHECK(request.capacity()==request.size());
  expected=native_asset_path(request);CHECK(compile_native_asset(request,disk,false));
  const auto largeReference=aurora::vita::gxm::prepare_texture_upload(desc,true,false);
  CHECK(largeReference.ok()&&largeReference.pixels.size()==4*1024*1024);
  denySize1=largeReference.pixels.size()+16;denySize2=largeReference.pixels.size();
  CHECK(load_native_texture(desc,native,false));
  denySize1=denySize2=0;
  CHECK(deniedAllocations==0&&native.pixels==largeReference.pixels);
  CHECK(native.pixels.data()==readerBuffer.load());
  // Allocation failure in request construction and in the optional reader
  // returns false through the noexcept entry point, without terminating GX.
  auto oomBefore=native_asset_stats().allocationFailures;
  denySize1=request.size();CHECK(!load_native_texture(desc,native,false));denySize1=0;
  CHECK(native_asset_stats().allocationFailures==oomBefore+1);
  CHECK(native.pixels==largeReference.pixels);
  failReader=true;CHECK(!load_native_texture(desc,native,false));failReader=false;
  CHECK(native_asset_stats().allocationFailures==oomBefore+2);
  CHECK(native.pixels==largeReference.pixels);
  request=native_geometry_request(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout);
  expected=native_asset_path(request);CHECK(compile_native_asset(request,disk));
  failReader=true;
  CHECK(!load_native_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,draw));
  failReader=false;CHECK(native_asset_stats().allocationFailures==oomBefore+3);
  CHECK(load_native_geometry(beIndices.data(),beIndices.size(),48,SourcePrimitive::Triangles,layout,draw));
  std::puts("large native payload ownership transfer and injected allocation failure fallback passed");
  configure_native_assets(nullptr,nullptr);
  std::puts("native asset exact source, payload checksum, bounds, immutable eligibility and layout invalidation passed");
}
