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
  for(unsigned i=0;i<draws;++i)stream.emplace_draw().pipelineKey=i+1;
  const auto before=allocations;
  for(unsigned frame=0;frame<frames;++frame) {
    stream.reset();
    for(unsigned i=0;i<draws;++i) {
      auto& packet=stream.emplace_draw();
      if(packet.pipelineKey||packet.fixedVertexUniforms)return 2;
      packet.pipelineKey=i+1;
    }
    if(stream.draw_packet(0).pipelineKey!=1||stream.tail_draw()->pipelineKey!=draws)return 3;
  }
  const size_t warmAllocations=allocations-before;
  std::printf("command stream: packet_bytes=%zu draws=%u frames=%u warm_allocations=%zu\n",
              sizeof(DrawPacket),draws,frames,warmAllocations);
  return warmAllocations?1:0;
}
