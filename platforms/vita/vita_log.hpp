#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace aurora::vita {

enum class RuntimeLogLevel : uint8_t {
  Silent = 0,
  Error = 1,
  Info = 2,
  Debug = 3,
};

inline std::atomic<uint8_t> g_runtimeLogLevel{
    static_cast<uint8_t>(RuntimeLogLevel::Info)};

inline void set_runtime_log_level(RuntimeLogLevel level) noexcept {
  g_runtimeLogLevel.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

inline RuntimeLogLevel runtime_log_level() noexcept {
  return static_cast<RuntimeLogLevel>(
      g_runtimeLogLevel.load(std::memory_order_relaxed));
}

inline bool runtime_log_enabled(RuntimeLogLevel level) noexcept {
  const auto requested = static_cast<uint8_t>(level);
  return requested != 0 &&
         g_runtimeLogLevel.load(std::memory_order_relaxed) >= requested;
}

inline void runtime_logf(RuntimeLogLevel level, const char* format, ...) noexcept {
  if (!runtime_log_enabled(level) || !format) return;
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
}

} // namespace aurora::vita

#define AURORA_VITA_LOG_ERROR(...) \
  ::aurora::vita::runtime_logf(::aurora::vita::RuntimeLogLevel::Error, __VA_ARGS__)
#define AURORA_VITA_LOG_INFO(...) \
  ::aurora::vita::runtime_logf(::aurora::vita::RuntimeLogLevel::Info, __VA_ARGS__)
#define AURORA_VITA_LOG_DEBUG(...) \
  ::aurora::vita::runtime_logf(::aurora::vita::RuntimeLogLevel::Debug, __VA_ARGS__)
