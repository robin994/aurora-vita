#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace aurora::vita::gfx {

// Equality, not lexicographic order. GX arrays are often unaligned; Vita's
// newlib memcmp falls back to a byte loop for them. Byte-vector loads preserve
// the full validation contract without requiring alignment or reading padding.
inline bool equal_memory_bytes(const void* left,const void* right,size_t bytes) noexcept {
  if(!bytes)return true;
  if(!left||!right)return false;
  if(left==right)return true;
  const auto* a=static_cast<const uint8_t*>(left);
  const auto* b=static_cast<const uint8_t*>(right);
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  const auto any_difference=[](uint8x16_t x) noexcept {
    const uint32x2_t words=vreinterpret_u32_u8(vorr_u8(vget_low_u8(x),vget_high_u8(x)));
    return (vget_lane_u32(words,0)|vget_lane_u32(words,1))!=0;
  };
  while(bytes>=64) {
    const uint8x16_t d0=veorq_u8(vld1q_u8(a),vld1q_u8(b));
    const uint8x16_t d1=veorq_u8(vld1q_u8(a+16),vld1q_u8(b+16));
    const uint8x16_t d2=veorq_u8(vld1q_u8(a+32),vld1q_u8(b+32));
    const uint8x16_t d3=veorq_u8(vld1q_u8(a+48),vld1q_u8(b+48));
    if(any_difference(vorrq_u8(vorrq_u8(d0,d1),vorrq_u8(d2,d3))))return false;
    a+=64;b+=64;bytes-=64;
  }
  while(bytes>=16) {
    if(any_difference(veorq_u8(vld1q_u8(a),vld1q_u8(b))))return false;
    a+=16;b+=16;bytes-=16;
  }
  while(bytes--)if(*a++!=*b++)return false;
  return true;
#else
  return std::memcmp(a,b,bytes)==0;
#endif
}

} // namespace aurora::vita::gfx
