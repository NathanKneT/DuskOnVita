#pragma once
#include <cstdint>
#include <cstring>
#include <limits>

namespace twv::vgl {
// GX integer attributes use a five-bit fractional shift (normal formats use
// fixed shifts6/14). Every supported reciprocal power of two is exactly an
// IEEE binary32 exponent. Avoid per-fetch integer conversion and VFP division.
inline float fixed_point_scale(uint32_t frac) {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    const uint32_t bits=frac<32 ? 0x3f800000u-(frac<<23) : 0x7f800000u;
    float result;std::memcpy(&result,&bits,sizeof result);return result;
}
}
