#pragma once
#include "exact_bytes.hpp"
#ifndef TWV_VITA_NEON_COMPARE
#define TWV_VITA_NEON_COMPARE 0
#endif
#if TWV_VITA_NEON_COMPARE && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace twv::vgl {
inline constexpr bool NeonCompareAvailable =
#if TWV_VITA_NEON_COMPARE && defined(__ARM_NEON)
    true;
#else
    false;
#endif

// Equality only. Byte loads accept every alignment and never exceed the span.
// ACLE v7 intrinsics: https://arm-software.github.io/acle/neon_intrinsics/advsimd.html
inline bool neon_bytes_equal(const void* left,const void* right,size_t bytes) noexcept {
#if TWV_VITA_NEON_COMPARE && defined(__ARM_NEON)
    auto* a=static_cast<const uint8_t*>(left);auto* b=static_cast<const uint8_t*>(right);
    const auto differs=[](uint8x16_t value) {
        const auto words=vreinterpretq_u32_u8(value);
        const auto halves=vorr_u32(vget_low_u32(words),vget_high_u32(words));
        return vget_lane_u32(vpmax_u32(halves,halves),0)!=0;
    };
    while(bytes>=32) {
        const auto first=veorq_u8(vld1q_u8(a),vld1q_u8(b));
        const auto second=veorq_u8(vld1q_u8(a+16),vld1q_u8(b+16));
        if(differs(vorrq_u8(first,second)))return false;
        a+=32;b+=32;bytes-=32;
    }
    if(bytes>=16) {
        if(differs(veorq_u8(vld1q_u8(a),vld1q_u8(b))))return false;
        a+=16;b+=16;bytes-=16;
    }
    return exact_bytes_equal(a,b,bytes);
#else
    return exact_bytes_equal(left,right,bytes);
#endif
}
} // namespace twv::vgl
