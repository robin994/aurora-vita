#pragma once
#include "vita_gfx_types.hpp"
#include <vector>

namespace aurora::vita::gfx {
// CPU reference for color EFB copies. Coordinates and row order are top-left.
// Input/output must not alias. Depth copies need a separate source representation.
bool copy_efb_rgba8(const uint8_t* source, uint32_t width, uint32_t height,
                    const Scissor& rect, uint32_t dstWidth, uint32_t dstHeight,
                    EfbCopyFormat format, bool flipX, bool flipY,
                    std::vector<uint8_t>& destination) noexcept;
}
