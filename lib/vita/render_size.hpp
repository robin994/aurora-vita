#pragma once
#include <cstdint>

namespace aurora::vita::render_size {

inline uint32_t g_renderWidth = 960;
inline uint32_t g_renderHeight = 544;
inline uint32_t g_frameWidth = 960;
inline uint32_t g_frameHeight = 544;

inline void configure(uint32_t renderWidth, uint32_t renderHeight,
                      uint32_t frameWidth, uint32_t frameHeight) noexcept {
  g_renderWidth = renderWidth;
  g_renderHeight = renderHeight;
  g_frameWidth = frameWidth;
  g_frameHeight = frameHeight;
}

} // namespace aurora::vita::render_size
