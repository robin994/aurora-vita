#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
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
// byte_spans_equal(). On Vita hash four words at once so large display-list
// records do not spend several milliseconds in scalar FNV every frame.
inline uint32_t byte_span_hash(const void* source, size_t bytes) noexcept {
  constexpr uint32_t Seed=0x9e3779b1u;
  constexpr uint32_t Prime=0x85ebca6bu;
  const auto* p=static_cast<const uint8_t*>(source);
  const uint64_t original=static_cast<uint64_t>(bytes);
  uint32_t h=Seed^static_cast<uint32_t>(original)^static_cast<uint32_t>(original>>32);
  if(!p)return h;
#if defined(__vita__) && defined(__ARM_NEON)
  uint32x4_t acc={0x243f6a88u,0x85a308d3u,0x13198a2eu,0x03707344u};
  const uint32x4_t mul=vdupq_n_u32(Prime);
  while(bytes>=16) {
    const auto words=vreinterpretq_u32_u8(vld1q_u8(p));
    acc=vmulq_u32(veorq_u32(acc,words),mul);
    acc=vextq_u32(acc,acc,1);
    p+=16;bytes-=16;
  }
  alignas(16) uint32_t lanes[4];
  vst1q_u32(lanes,acc);
  h^=lanes[0]+0x9e3779b9u;
  h=(h^lanes[1])*Prime;
  h=(h^lanes[2])*Prime;
  h=(h^lanes[3])*Prime;
#else
  while(bytes>=4) {
    uint32_t word;
    std::memcpy(&word,p,sizeof(word));
    h=(h^word)*Prime;
    p+=4;bytes-=4;
  }
#endif
  while(bytes--)h=(h^*p++)*Prime;
  h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;h^=h>>16;
  return h;
}

} // namespace aurora::vita::gfx
