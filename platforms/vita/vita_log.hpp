#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace aurora::vita {

#ifndef AURORA_VITA_RUNTIME_LOGGING
#define AURORA_VITA_RUNTIME_LOGGING 1
#endif

enum class RuntimeLogLevel : uint8_t {
  Silent = 0,
  Error = 1,
  Info = 2,
  Debug = 3,
};

#if AURORA_VITA_RUNTIME_LOGGING
inline std::atomic<uint8_t> g_runtimeLogLevel{static_cast<uint8_t>(RuntimeLogLevel::Info)};

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

inline void runtime_logf_unchecked(const char* format, ...) noexcept {
  if (!format) return;
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
}
#else
inline void set_runtime_log_level(RuntimeLogLevel) noexcept {}
inline RuntimeLogLevel runtime_log_level() noexcept { return RuntimeLogLevel::Silent; }
inline constexpr bool runtime_log_enabled(RuntimeLogLevel) noexcept { return false; }
inline void runtime_logf(RuntimeLogLevel, const char*, ...) noexcept {}
inline void runtime_logf_unchecked(const char*, ...) noexcept {}
#endif

} // namespace aurora::vita

#if AURORA_VITA_RUNTIME_LOGGING
#define AURORA_VITA_LOG_AT(level_, ...) do { \
  if (::aurora::vita::runtime_log_enabled(level_)) \
    ::aurora::vita::runtime_logf_unchecked(__VA_ARGS__); \
} while (false)

#define AURORA_VITA_LOG_ERROR(...) \
  AURORA_VITA_LOG_AT(::aurora::vita::RuntimeLogLevel::Error, __VA_ARGS__)
#define AURORA_VITA_LOG_INFO(...) \
  AURORA_VITA_LOG_AT(::aurora::vita::RuntimeLogLevel::Info, __VA_ARGS__)
#define AURORA_VITA_LOG_DEBUG(...) \
  AURORA_VITA_LOG_AT(::aurora::vita::RuntimeLogLevel::Debug, __VA_ARGS__)
#else
#define AURORA_VITA_LOG_ERROR(...) do {} while (false)
#define AURORA_VITA_LOG_INFO(...) do {} while (false)
#define AURORA_VITA_LOG_DEBUG(...) do {} while (false)
#endif
