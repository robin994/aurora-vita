#include "gfx/vita_command_stream.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {size_t allocations=0;}
void* operator new(size_t bytes) {
  ++allocations;
  if(void* result=std::malloc(bytes?bytes:1))return result;
  std::abort();
}
void operator delete(void* ptr) noexcept {std::free(ptr);}
void operator delete(void* ptr,size_t) noexcept {std::free(ptr);}

int main() {
  using namespace aurora::vita::gfx;
  CommandStream stream;
  constexpr unsigned draws=2048,frames=20;
  stream.reserve(draws);
  DrawUniforms uniforms{};std::array<TextureBinding,MaxTextures> textures{};
  for(unsigned i=0;i<draws;++i) {
    auto& packet=stream.emplace_draw();packet.pipelineKey=i+1;
    uniforms.mvp[0]=float(i);stream.share_draw_state(packet,uniforms,textures,i+1);
  }
  const auto before=allocations;
  for(unsigned frame=0;frame<frames;++frame) {
    stream.reset();
    for(unsigned i=0;i<draws;++i) {
      auto& packet=stream.emplace_draw();
      if(packet.pipelineKey||packet.fixedVertexUniforms)return 2;
      packet.pipelineKey=i+1;
      uniforms.mvp[0]=float(i);stream.share_draw_state(packet,uniforms,textures,i+1);
    }
    if(stream.draw_packet(0).pipelineKey!=1||stream.tail_draw()->pipelineKey!=draws)return 3;
    if(stream.state_snapshot_count()!=draws||stream.draw_packet(0).gpu_uniforms().mvp[0]!=0)return 4;
  }
  const size_t warmAllocations=allocations-before;
  std::printf("command stream: packet_bytes=%zu draws=%u frames=%u warm_allocations=%zu\n",
              sizeof(DrawPacket),draws,frames,warmAllocations);
  // A6 same-frame native replay deep-copies the previous packet's resolved
  // draw-state snapshot without retaining a pointer to a mutable producer.
  stream.reset();
  FixedVertexUniforms fixed{};
  fixed.position[0]=7.f;
  auto& source=stream.emplace_geometry_draw();
  source.pipelineKey=0x123456;
  source.vertices={17,0,48};source.indices={18,0,6};
  source.vertexCount=3;source.indexCount=3;source.fixedVertexUniforms=&fixed;
  uniforms.mvp[0]=42.f;textures[0].texture=44;
  stream.share_draw_state(source,uniforms,textures,143,47);
  stream.draw(*stream.tail_draw());
  if(stream.size()!=2||stream.tail_draw()==&source)return 5;
  const auto& copied=*stream.tail_draw();
  if(copied.pipelineKey!=source.pipelineKey||copied.vertices.buffer!=17||
     copied.indices.buffer!=18||copied.fixedVertexUniforms!=&fixed||
     copied.gpu_uniforms().mvp[0]!=42.f||copied.texture_bindings()[0].texture!=44||
     copied.uniformRevision!=143||copied.textureBindingRevision!=47||
     copied.sharedState)return 6;
  uniforms.mvp[0]=11.f;textures[0].texture=100;
  if(copied.gpu_uniforms().mvp[0]!=42.f||copied.texture_bindings()[0].texture!=44)return 7;
  return warmAllocations?1:0;
}
