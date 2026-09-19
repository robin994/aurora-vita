#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#if defined(__vita__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace aurora::vita::gfx {

// GX arrays may start at any byte boundary. Unlike a lexicographic memcmp,
// cache validation only needs equality. Never read beyond the supplied span.
inline bool byte_spans_equal(const void* lhs, const void* rhs, size_t bytes) noexcept {
  if (bytes == 0 || lhs == rhs) return true;
  if (!lhs || !rhs) return false;
#if defined(__vita__) && defined(__ARM_NEON)
  auto* a = static_cast<const uint8_t*>(lhs);
  auto* b = static_cast<const uint8_t*>(rhs);
  const auto nonzero = [](uint8x16_t value) noexcept {
    const uint32x4_t words = vreinterpretq_u32_u8(value);
    const uint32x2_t halves = vorr_u32(vget_low_u32(words), vget_high_u32(words));
    return (vget_lane_u32(halves, 0) | vget_lane_u32(halves, 1)) != 0;
  };
  while (bytes >= 64) {
    const auto d0 = veorq_u8(vld1q_u8(a), vld1q_u8(b));
    const auto d1 = veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16));
    const auto d2 = veorq_u8(vld1q_u8(a + 32), vld1q_u8(b + 32));
    const auto d3 = veorq_u8(vld1q_u8(a + 48), vld1q_u8(b + 48));
    if (nonzero(vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3)))) return false;
    a += 64; b += 64; bytes -= 64;
  }
  while (bytes >= 16) {
    if (nonzero(veorq_u8(vld1q_u8(a), vld1q_u8(b)))) return false;
    a += 16; b += 16; bytes -= 16;
  }
  while (bytes--) if (*a++ != *b++) return false;
  return true;
#else
  return std::memcmp(lhs, rhs, bytes) == 0;
#endif
}

// Candidate lookup only: exact cache reuse is still guarded by
// byte_spans_equal(). XXH3 replaces the previous scalar mixer because this
// runs on large immutable display-list records in the geometry-cache hot path.
inline uint64_t byte_span_hash(const void* source, size_t bytes) noexcept {
  static constexpr uint8_t Empty=0;
  return XXH3_64bits(source?source:&Empty,source?bytes:0);
}

} // namespace aurora::vita::gfx
