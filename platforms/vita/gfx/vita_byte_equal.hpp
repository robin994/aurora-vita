#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace aurora::vita::gfx {

// Equality only: unlike memcmp, no ordering or first differing byte is needed.
// GX arrays are commonly unaligned. Vita's libc falls back to a byte loop for
// those arrays; NEON byte loads accept their alignment without reading past n.
inline bool byte_equal(const void* lhs,const void* rhs,size_t n) noexcept {
  const auto* a=static_cast<const uint8_t*>(lhs);
  const auto* b=static_cast<const uint8_t*>(rhs);
  if(a==b||n==0)return true;
  if(!a||!b)return false;
#if defined(__ARM_NEON)
  while(n>=64) {
    const auto d0=veorq_u8(vld1q_u8(a),vld1q_u8(b));
    const auto d1=veorq_u8(vld1q_u8(a+16),vld1q_u8(b+16));
    const auto d2=veorq_u8(vld1q_u8(a+32),vld1q_u8(b+32));
    const auto d3=veorq_u8(vld1q_u8(a+48),vld1q_u8(b+48));
    const auto d=vorrq_u8(vorrq_u8(d0,d1),vorrq_u8(d2,d3));
    const auto folded=vreinterpret_u64_u8(vorr_u8(vget_low_u8(d),vget_high_u8(d)));
    if(vget_lane_u64(folded,0)!=0)return false;
    a+=64;b+=64;n-=64;
  }
  while(n>=16) {
    const auto d=veorq_u8(vld1q_u8(a),vld1q_u8(b));
    const auto folded=vreinterpret_u64_u8(vorr_u8(vget_low_u8(d),vget_high_u8(d)));
    if(vget_lane_u64(folded,0)!=0)return false;
    a+=16;b+=16;n-=16;
  }
#else
  while(n>=sizeof(uint64_t)) {
    uint64_t x,y;
    std::memcpy(&x,a,sizeof(x));std::memcpy(&y,b,sizeof(y));
    if(x!=y)return false;
    a+=sizeof(x);b+=sizeof(y);n-=sizeof(x);
  }
#endif
  while(n--)if(*a++!=*b++)return false;
  return true;
}

} // namespace aurora::vita::gfx
