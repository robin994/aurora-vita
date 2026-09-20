#include "aurora_vita_backend.hpp"
#include "gfx/vita_command_stream.hpp"
#include "gfx/vita_streaming_arena.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gxm/gxm_texture_layout.hpp"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
using namespace aurora::vita;
using namespace gfx;
unsigned checks=0,failures=0;
void check(bool ok,const char* expression,int line) {
  ++checks;
  if(!ok){++failures;std::fprintf(stderr,"FAIL %d: %s\n",line,expression);}
}
#define CHECK(x) check(bool(x),#x,__LINE__)

bool same_vertex(const CanonicalVertex& a,const CanonicalVertex& b) {
  return std::memcmp(a.position,b.position,sizeof(a.position))==0&&
      std::memcmp(a.normal,b.normal,sizeof(a.normal))==0&&
      std::memcmp(a.binormal,b.binormal,sizeof(a.binormal))==0&&
      std::memcmp(a.tangent,b.tangent,sizeof(a.tangent))==0&&
      std::memcmp(a.color0,b.color0,sizeof(a.color0))==0&&
      std::memcmp(a.color1,b.color1,sizeof(a.color1))==0&&
      std::memcmp(a.texcoord,b.texcoord,sizeof(a.texcoord))==0&&
      a.pnMatrixIndex==b.pnMatrixIndex&&
      std::memcmp(a.texMatrixIndex,b.texMatrixIndex,sizeof(a.texMatrixIndex))==0;
}

void compiled_decode_equivalence() {
  // Exercise the actual compiled decoder against its reference path, including
  // 4-component inputs that previously overwrote W/adjacent canonical fields.
  std::array<uint8_t,64> data{};
  for(unsigned i=0;i<data.size();++i)data[i]=uint8_t(1+i%13);
  for(unsigned s=0;s<=unsigned(VertexSemantic::Tex7);++s) {
    if(s==unsigned(VertexSemantic::Color0)||s==unsigned(VertexSemantic::Color1))continue;
    for(unsigned c=0;c<=unsigned(VertexComponent::F32);++c)
      for(unsigned n=1;n<=4;++n)for(bool little:{false,true})
        for(auto source:{VertexSource::Direct,VertexSource::Index8,VertexSource::Index16}) {
          // Matrix selectors are single unsigned bytes in valid GX streams.
          if(s<unsigned(VertexSemantic::Position)&&(c!=0||n!=1))continue;
          VertexDecodeLayout layout{};layout.count=1;layout.streamStride=32;layout.streamLittleEndian=little;
          auto& a=layout.attributes[0];a.semantic=VertexSemantic(s);a.source=source;
          a.component=VertexComponent(c);a.components=n;a.frac=3;
          a.valueOffset=3;a.array={data.data(),data.size(),32,little};
          std::array<uint8_t,32> indices{};
          const auto* stream=source==VertexSource::Direct?data.data():indices.data();
          CanonicalVertex reference{},actual{};
          const bool expected=decode_vertex_into(stream,32,0,layout,reference);
          compile_vertex_decode_layout(layout);
          const bool result=decode_vertex_into(stream,32,0,layout,actual);
          CHECK(result==expected);
          CHECK(!result||same_vertex(reference,actual));
        }
  }
  VertexDecodeLayout layout{};layout.count=1;layout.streamStride=32;
  layout.attributes[0]={VertexSemantic::Normal,VertexSource::Direct,VertexComponent::F32,0};
  compile_vertex_decode_layout(layout);
  CanonicalVertex v{};
  CHECK(!decode_vertex_into(data.data(),data.size(),0,layout,v));
  layout.attributes[0].components=5;compile_vertex_decode_layout(layout);
  CHECK(!decode_vertex_into(data.data(),data.size(),0,layout,v));
  layout.attributes[0].components=3;compile_vertex_decode_layout(layout);
  CHECK(!decode_vertex_into(data.data(),11,0,layout,v));
  layout.opCount=25;
  CHECK(!decode_vertex_into(data.data(),data.size(),0,layout,v));
}

void arena_boundaries() {
  BufferPool pool;
  StreamingArenaConfig config{};config.vertexBytes=17;config.indexBytes=17;config.slots=1;
  StreamingArena arena(pool,config);
  CHECK(arena.initialize());arena.begin_frame(0);
  CHECK(arena.can_reserve(16,1,16,1));
  CHECK(!arena.can_reserve(17,1,0,1));
  CHECK(!arena.can_reserve(0,1,17,1));
  void* data=nullptr;
  CHECK(arena.reserve_vertices(16,1,&data).buffer!=0&&data);
  CHECK(!arena.can_reserve(1,1,0,1));
  CHECK(arena.reserve_vertices(1,1,&data).buffer==0&&!data);
  CHECK(arena.vertex_used()==16);
}

void bounded_mips() {
  std::array<uint8_t,64> data{};
  TextureDesc desc{};desc.width=desc.height=4;desc.format=TextureFormat::RGBA8888;
  desc.data=data.data();desc.dataSize=0;
  CHECK(!gxm::prepare_swizzled_texture(desc).ok());
  desc.dataSize=data.size()-1;
  CHECK(!gxm::prepare_swizzled_texture(desc).ok());
  desc.dataSize=data.size();
  CHECK(gxm::prepare_swizzled_texture(desc).ok());
  desc.data=nullptr;
  CHECK(!gxm::prepare_swizzled_texture(desc).ok());
}

void command_lifetimes() {
  CommandStream stream;
  auto* first=&stream.emplace_draw();first->pipelineKey=123;first->uniforms.mvp[0]=7;
  stream.barrier();
  for(unsigned i=0;i<2048;++i)stream.emplace_draw().pipelineKey=1000+i;
  CHECK(first->pipelineKey==123&&first->uniforms.mvp[0]==7);
  CHECK(&stream.draw_packet(stream.commands()[0].drawIndex)==first);
  CHECK(stream.commands()[1].type==CommandType::Barrier);
  stream.reset();
  CHECK(stream.size()==0&&stream.tail_draw()==nullptr);
  auto& next=stream.emplace_draw();
  CHECK(next.pipelineKey==0&&next.fixedVertexUniforms==nullptr);
  CHECK(next.textures[0].uvScaleX==1.f&&next.textures[0].uvScaleY==1.f);
}
}
int main() {
  compiled_decode_equivalence();arena_boundaries();bounded_mips();command_lifetimes();
  const aurora::vita::BackendConfig defaults{};
#if defined(AURORA_VITA_RENDERER_GXM)
  CHECK(defaults.static_geometry_budget==8*1024*1024);
  CHECK(defaults.gxm_lit_fixed_vertex_gpu);
#else
  CHECK(defaults.static_geometry_budget==0);
  CHECK(!defaults.gxm_lit_fixed_vertex_gpu);
#endif
  CHECK(defaults.render_width==0&&defaults.render_height==0);
  CHECK(!defaults.gxm_d16_depth);
  std::printf("vita regression: %u checks, %u failures\n",checks,failures);
  return failures?1:0;
}
