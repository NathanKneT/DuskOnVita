#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifndef TWV_VITA_WORD_COMPARE
#define TWV_VITA_WORD_COMPARE 1
#endif

namespace twv::vgl {
// Equality only, not lexicographic ordering. Fixed-size memcpy loads preserve
// C++ alignment/aliasing rules. Never read beyond either requested span.
inline bool exact_bytes_equal(const void* left,const void* right,size_t bytes) noexcept {
    if(!bytes)return true;
#if TWV_VITA_WORD_COMPARE
    auto* a=static_cast<const uint8_t*>(left);auto* b=static_cast<const uint8_t*>(right);
    while(bytes>=16) {
        uint32_t a0,a1,a2,a3,b0,b1,b2,b3;
        std::memcpy(&a0,a,4);std::memcpy(&b0,b,4);
        std::memcpy(&a1,a+4,4);std::memcpy(&b1,b+4,4);
        std::memcpy(&a2,a+8,4);std::memcpy(&b2,b+8,4);
        std::memcpy(&a3,a+12,4);std::memcpy(&b3,b+12,4);
        if((a0^b0)|(a1^b1)|(a2^b2)|(a3^b3))return false;
        a+=16;b+=16;bytes-=16;
    }
    while(bytes>=4) {
        uint32_t av,bv;std::memcpy(&av,a,4);std::memcpy(&bv,b,4);
        if(av!=bv)return false;
        a+=4;b+=4;bytes-=4;
    }
    while(bytes--)if(*a++!=*b++)return false;
    return true;
#else
    return std::memcmp(left,right,bytes)==0;
#endif
}
}
