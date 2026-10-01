#pragma once

#include <array>
#include <cstdint>

namespace aurora::gx::fifo {

// Producer-side state of emitted BP commands, independent of decoded GX state.
// Only material registers with no action/alias semantics may be elided.
class BpWriteCache {
  std::array<uint32_t, 256> values_{};
  std::array<bool, 256> valid_{};
  bool fullMask_ = false;

public:
  static constexpr bool cacheable(uint8_t reg) noexcept {
    return (reg >= 0x10 && reg <= 0x1f) || // TEV indirect stage configuration
           (reg >= 0x25 && reg <= 0x2f) || // indirect scale/reference and TEV order
           (reg >= 0xc0 && reg <= 0xdf) || // TEV color/alpha combiners
           (reg >= 0xf6 && reg <= 0xfd);   // TEV konst selection and swap tables
  }

  // A raw list can leave any BP value and even a pending one-shot mask behind.
  void invalidate() noexcept {
    valid_.fill(false);
    fullMask_ = false;
  }

  bool should_emit(uint32_t value) noexcept {
    const uint8_t reg = static_cast<uint8_t>(value >> 24);
    if (reg == 0xfe) {
      fullMask_ = (value & 0x00ffffffu) == 0x00ffffffu;
      return true;
    }

    const bool fullWrite = fullMask_;
    // Every non-mask BP write consumes the previous mask, even if its merged
    // register value does not change. Never elide such a masked write.
    fullMask_ = true;
    if (!cacheable(reg)) return true;
    if (!fullWrite) {
      valid_[reg] = false;
      return true;
    }
    if (valid_[reg] && values_[reg] == value) return false;
    values_[reg] = value;
    valid_[reg] = true;
    return true;
  }
};

} // namespace aurora::gx::fifo
