#pragma once
#include <cstdlib>
#include <cstring>

namespace aurora::gx::fifo {
inline bool native_model_prepared_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("STRIKERS_GXM_NATIVE_MODEL_PREPARED");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

// R3-A: bypass GLX's unchanged draw callback only for a fully warmed and
// registered native recipe. The original GX path stays the default.
inline bool native_model_early_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("STRIKERS_GXM_NATIVE_MODEL_EARLY");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}
} // namespace aurora::gx::fifo
