#pragma once

#include <cstdint>
#include <limits>

#include <psp2/kernel/threadmgr.h>

namespace aurora::vita::thread {

inline int delay_us(int64_t requestedUs) noexcept {
  if (requestedUs <= 0) return 0;
  const uint64_t clamped = static_cast<uint64_t>(requestedUs) > std::numeric_limits<uint32_t>::max()
      ? std::numeric_limits<uint32_t>::max()
      : static_cast<uint64_t>(requestedUs);
  return sceKernelDelayThread(static_cast<unsigned int>(clamped));
}

} // namespace aurora::vita::thread
