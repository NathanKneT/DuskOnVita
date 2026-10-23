#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace twv::vgl {
template<unsigned Bits>
inline constexpr auto ColorUnorm=[] {
    static_assert(Bits==4 || Bits==5 || Bits==6 || Bits==8);
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    std::array<float,1u<<Bits> values{};
    for(unsigned i=0;i<values.size();++i)values[i]=float(i)/float(values.size()-1);
    return values;
}();
// Inputs are already masked to the packed GX component width. The table uses
// the same binary32 division as the previous decoder, evaluated at compile time
// under the port's default IEEE round-to-nearest environment. All four tables
// together use1472B of read-only storage; no color quantization is introduced.
template<unsigned Bits>
inline float normalized_color(uint32_t value) {
#if TWV_VITA_COLOR_LOOKUP
    return ColorUnorm<Bits>[value];
#else
    return float(value)/float((1u<<Bits)-1);
#endif
}
}
