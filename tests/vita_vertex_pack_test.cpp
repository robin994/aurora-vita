#include "gfx/vita_vertex_pack.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace aurora::vita::gfx;
static void reference(uint8_t* dst, const CanonicalVertex& src, const VertexLayout& layout) {
  for(unsigned i=0;i<layout.count;++i) {
    const auto& a=layout.attributes[i];
    if(a.location==0)std::memcpy(dst+a.offset,src.position,16);
    else if(a.location==1)std::memcpy(dst+a.offset,src.color0,4);
    else if(a.location==2)std::memcpy(dst+a.offset,src.color1,4);
    else if(a.location>=3&&a.location<11)std::memcpy(dst+a.offset,src.texcoord[a.location-3],12);
    else if(a.location==11)std::memcpy(dst+a.offset,src.normal,12);
    else if(a.location==12)std::memcpy(dst+a.offset,src.binormal,12);
    else if(a.location==13)std::memcpy(dst+a.offset,src.tangent,12);
  }
}
extern "C" int test_vertex_pack() {
  CanonicalVertex vertex;
  auto* bytes=reinterpret_cast<uint8_t*>(&vertex);
  for(size_t i=0;i<sizeof(vertex);++i)bytes[i]=uint8_t(i*79u+31u);
  unsigned checks=0;
  for(unsigned mask=0;mask<256;++mask)for(unsigned color=0;color<4;++color)
    for(unsigned alignment=0;alignment<8;++alignment) {
      auto layout=gpu_vertex_layout(uint8_t(mask),uint8_t(color));
      alignas(16) std::array<uint8_t,256> expected,actual;
      expected.fill(0xCD);actual=expected;
      reference(expected.data()+16+alignment,vertex,layout);
      pack_gpu_vertex_inline(actual.data()+16+alignment,vertex,layout);
      if(actual!=expected)return 1;
      ++checks;
    }
  VertexLayout extra{};
  extra.count=3;
  for(unsigned i=0;i<3;++i)extra.attributes[i]={uint8_t(11+i),3,VertexScalar::F32,false,40,uint16_t(i*12+1)};
  std::array<uint8_t,64> expected{},actual{};
  reference(expected.data(),vertex,extra);
  pack_gpu_vertex_inline(actual.data(),vertex,extra);
  if(actual!=expected)return 2;
  std::printf("PASS vertex packing: %u layouts/alignments, exact bit copies, NBT and untouched guards\n",checks+1);
  return 0;
}
#ifndef MV_VERTEX_PACK_NO_MAIN
int main() { return test_vertex_pack(); }
#endif
