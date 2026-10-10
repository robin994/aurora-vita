#pragma once
#include "gfx/vita_gfx_types.hpp"
#include <algorithm>

namespace aurora::vita::gxm {
// Native viewport Y scale is negative. Clip-space top (+1) samples row zero
// of every linear GXM surface, including offscreen EFBs and display aliases.
inline constexpr float EfbCopyVertices[]{
    -1,-1,-.5f,1, 0,1,1,
     3,-1,-.5f,1, 2,1,1,
    -1, 3,-.5f,1, 0,-1,1};

struct EfbClearArea {
  gfx::Scissor rect{};
  bool fullTarget=false;
};
inline EfbClearArea efb_clear_area(const gfx::Scissor* source,uint32_t width,uint32_t height) noexcept {
  gfx::Scissor rect{0,0,int32_t(width),int32_t(height)};
  if(source) {
    const int64_t x=std::clamp<int64_t>(source->x,0,width);
    const int64_t y=std::clamp<int64_t>(source->y,0,height);
    const int64_t right=std::clamp<int64_t>(int64_t(source->x)+std::max(source->width,0),x,width);
    const int64_t bottom=std::clamp<int64_t>(int64_t(source->y)+std::max(source->height,0),y,height);
    rect={int32_t(x),int32_t(y),int32_t(right-x),int32_t(bottom-y)};
  }
  return {rect,rect.x==0 && rect.y==0 && rect.width==int32_t(width) && rect.height==int32_t(height)};
}

inline gfx::DrawPacket efb_clear_packet(uint64_t key,gfx::Handle vertices,gfx::Handle indices,
    uint32_t width,uint32_t height,const EfbClearArea& area,const gfx::Color& color,float depth) noexcept {
  gfx::DrawPacket packet{};
  packet.pipelineKey=key;
  packet.vertices={vertices,0,48};packet.indices={indices,0,6};
  packet.vertexCount=packet.indexCount=3;
  packet.viewport.width=float(width);packet.viewport.height=float(height);
  packet.scissor=area.rect;
  packet.uniforms.kcolor[0]={color.r,color.g,color.b,color.a};
  packet.uniforms.mvp[14]=depth-1.f;
  return packet;
}
} // namespace aurora::vita::gxm
