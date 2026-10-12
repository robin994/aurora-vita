#pragma once
#include "vita_gfx_types.hpp"
#include <vector>

namespace aurora::vita::gfx {
// A fully clipped EFB source contains no visible samples. Unlike a partial
// overlap, its copy can be represented by a zero-initialized destination.
inline constexpr bool efb_copy_source_empty(const Scissor& source,uint32_t width,uint32_t height) noexcept {
  return source.width<=0||source.height<=0||source.x>=int64_t(width)||source.y>=int64_t(height)||
      int64_t(source.x)+source.width<=0||int64_t(source.y)+source.height<=0;
}
// Only the physical capture source is cropped. GXCopyTex(clear=true) must use
// the full original scissor after the copy or the guest observes stale pixels.
// Pixel-center filtering differs from shader scaling: this is opt-in only.
inline constexpr bool efb_snap_one_to_one_capture(const Scissor& source,
    uint32_t guestSourceWidth,uint32_t guestSourceHeight,
    uint32_t guestDestWidth,uint32_t guestDestHeight,
    uint32_t physicalDestWidth,uint32_t physicalDestHeight,
    uint32_t targetWidth,uint32_t targetHeight,Scissor& capture) noexcept {
  capture=source;
  if(!guestSourceWidth||!guestSourceHeight||
     guestSourceWidth!=guestDestWidth||guestSourceHeight!=guestDestHeight||
     !physicalDestWidth||!physicalDestHeight||
     physicalDestWidth>uint32_t(INT32_MAX)||physicalDestHeight>uint32_t(INT32_MAX)||
     source.x<0||source.y<0||source.width<=0||source.height<=0)return false;
  const auto w=uint32_t(source.width),h=uint32_t(source.height);
  if(w<physicalDestWidth||w-physicalDestWidth>1u||
     h<physicalDestHeight||h-physicalDestHeight>1u||
     (w==physicalDestWidth&&h==physicalDestHeight)||
     uint64_t(source.x)+w>targetWidth||uint64_t(source.y)+h>targetHeight)return false;
  capture.width=int32_t(physicalDestWidth);
  capture.height=int32_t(physicalDestHeight);
  return true;
}
// CPU reference for color EFB copies. Coordinates and row order are top-left.
// Input/output must not alias. Depth copies need a separate source representation.
bool copy_efb_rgba8(const uint8_t* source, uint32_t width, uint32_t height,
                    const Scissor& rect, uint32_t dstWidth, uint32_t dstHeight,
                    EfbCopyFormat format, bool flipX, bool flipY,
                    std::vector<uint8_t>& destination) noexcept;
}
