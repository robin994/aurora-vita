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
// CPU reference for color EFB copies. Coordinates and row order are top-left.
// Input/output must not alias. Depth copies need a separate source representation.
bool copy_efb_rgba8(const uint8_t* source, uint32_t width, uint32_t height,
                    const Scissor& rect, uint32_t dstWidth, uint32_t dstHeight,
                    EfbCopyFormat format, bool flipX, bool flipY,
                    std::vector<uint8_t>& destination) noexcept;
}
