#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aurora::vita::gfx {
// SamplerDesc stores LOD bias in mip levels. GXM uses an unsigned six-bit field
// centered on 31, in eighth-level steps. Keep that encoding at the device edge.
inline uint32_t native_lod_bias(float levels) noexcept {
  if(!std::isfinite(levels))return 31;
  return static_cast<uint32_t>(std::floor(std::clamp(levels*8.f+31.f,0.f,63.f)+.5f));
}
// vitaGL's LOD_BIAS setter adds 31 to its integer argument without scaling it.
inline float vitagl_lod_bias(float levels) noexcept {
  return static_cast<float>(static_cast<int>(native_lod_bias(levels))-31);
}
}
