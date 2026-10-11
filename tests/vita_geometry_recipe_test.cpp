#include "gfx/vita_static_geometry.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace aurora::vita::gfx;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main(){
  std::array<float,9> positions{1,2,3,4,5,6,7,8,9};std::array<uint8_t,60> raw{};
  for(unsigned i=0;i<raw.size();++i)raw[i]=i%3;
  VertexDecodeLayout layout;layout.count=1;layout.streamStride=1;
  layout.attributes[0]={VertexSemantic::Position,VertexSource::Index8,VertexComponent::F32,3,0,0,0,
    {reinterpret_cast<const uint8_t*>(positions.data()),sizeof positions,12,true}};
  PipelineDesc p;p.fixedVertexOnGpu=true;VertexTransformState state;
  Renderer renderer;CHECK(renderer.initialize());StaticGeometryCache cache(renderer,1024*1024);
  auto recipe=build_draw_recipe(p);
  note_memory_write(raw.data(),raw.size());note_memory_write(positions.data(),sizeof positions);
  auto* first=cache.get(raw.data(),raw.size(),raw.size(),SourcePrimitive::Triangles,layout,p,state,nullptr,raw.data());CHECK(first);
  const auto handle=first->vertices.buffer;
  for(unsigned i=0;i<1000;++i){state.postexMatrices[0].v[3]=float(i);
    auto* hit=cache.get(raw.data(),raw.size(),raw.size(),SourcePrimitive::Triangles,layout,p,state,nullptr,raw.data(),nullptr,0,&recipe);
    CHECK(hit&&hit->vertices.buffer==handle);}
  CHECK(cache.hits()==1000);CHECK(cache.size()==1);
  positions[0]+=1;note_memory_write(positions.data(),sizeof positions);
  CHECK(!cache.get(raw.data(),raw.size(),raw.size(),SourcePrimitive::Triangles,layout,p,state,nullptr,raw.data(),nullptr,0,&recipe));
  CHECK(cache.hits()==1000);cache.clear();CHECK(cache.bytes()==0);
  // Source unload removes the CPU lookup before the same allocation is reused.
  // Queued GPU resources remain charged until the existing retirement fence.
  cache.begin_frame(1);
  first=cache.get(raw.data(),raw.size(),raw.size(),SourcePrimitive::Triangles,layout,p,state,nullptr,raw.data());CHECK(first);
  const auto oldHandle=first->vertices.buffer;
  const auto oldKey=first->replayKey;
  CHECK(cache.invalidate_source_range(raw.data(),raw.size())==1);
  CHECK(cache.size()==0 && cache.retired_bytes()>0);
  CHECK(!cache.validate_native_model(oldKey,first));
  auto* reloaded=cache.get(raw.data(),raw.size(),raw.size(),SourcePrimitive::Triangles,layout,p,state,nullptr,raw.data());
  CHECK(reloaded && reloaded->vertices.buffer!=oldHandle);
  cache.begin_frame(StaticGeometryCache::RetireFrames);
  CHECK(cache.retired_bytes()>0);
  cache.begin_frame(1+StaticGeometryCache::RetireFrames);
  CHECK(cache.retired_bytes()==0);
  // An indexed source range invalidates its mesh too; unrelated ranges do not.
  CHECK(cache.invalidate_source_range(nullptr,0)==0);
  CHECK(cache.invalidate_source_range(positions.data(),sizeof positions)==1);
  cache.clear();CHECK(cache.bytes()==0);
  puts("prepared geometry recipe: same resident object, camera changes, tracked mutation, resource release");
}
