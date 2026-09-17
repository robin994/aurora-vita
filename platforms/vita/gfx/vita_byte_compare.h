#ifndef AURORA_VITA_BYTE_COMPARE_H
#define AURORA_VITA_BYTE_COMPARE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* Equality, not lexicographic ordering. Exact byte checks are deliberately
 * retained instead of probabilistic content hashes. Every load is bounded by
 * size, including unaligned buffers and the final short tail. */
static inline int aurora_vita_bytes_equal(const void *left,const void *right,size_t size)
{
    const uint8_t *a=(const uint8_t *)left,*b=(const uint8_t *)right;
    if(a==b || !size) return 1;
#if defined(__ARM_NEON)
    /* Reduce in NEON before crossing to an ARM register. Extracting two u64
     * lanes made the ARMv7 compiler spill and reload the vector on every 16
     * bytes. Four-vector chunks also amortize the SIMD-to-scalar dependency. */
    while(size>=64) {
        uint8x16_t difference=veorq_u8(vld1q_u8(a),vld1q_u8(b));
        difference=vorrq_u8(difference,veorq_u8(vld1q_u8(a+16),vld1q_u8(b+16)));
        difference=vorrq_u8(difference,veorq_u8(vld1q_u8(a+32),vld1q_u8(b+32)));
        difference=vorrq_u8(difference,veorq_u8(vld1q_u8(a+48),vld1q_u8(b+48)));
        const uint32x4_t words=vreinterpretq_u32_u8(difference);
        const uint32x2_t pair=vorr_u32(vget_low_u32(words),vget_high_u32(words));
        if(vget_lane_u32(vpmax_u32(pair,pair),0)) return 0;
        a+=64;b+=64;size-=64;
    }
    while(size>=16) {
        const uint8x16_t difference=veorq_u8(vld1q_u8(a),vld1q_u8(b));
        const uint32x4_t words=vreinterpretq_u32_u8(difference);
        const uint32x2_t pair=vorr_u32(vget_low_u32(words),vget_high_u32(words));
        if(vget_lane_u32(vpmax_u32(pair,pair),0)) return 0;
        a+=16;b+=16;size-=16;
    }
    if(size>=8) {
        const uint32x2_t words=vreinterpret_u32_u8(veor_u8(vld1_u8(a),vld1_u8(b)));
        if(vget_lane_u32(vpmax_u32(words,words),0)) return 0;
        a+=8;b+=8;size-=8;
    }
#else
    // Keep the portable word path where memcpy can safely lower unaligned
    // reads. On strict-alignment ARMv7 the short tail below avoids libc calls.
    while(size>=4) {
        uint32_t x,y;
        memcpy(&x,a,4);memcpy(&y,b,4);
        if(x!=y) return 0;
        a+=4;b+=4;size-=4;
    }
#endif
    while(size--) if(*a++!=*b++) return 0;
    return 1;
}
#endif
