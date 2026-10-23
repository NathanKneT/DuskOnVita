#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
namespace twv::vgl {
inline bool rgba_chain_size(uint32_t w,uint32_t h,uint32_t levels,size_t& result) noexcept {
    result=0;
    if(!w||!h||!levels||levels>32) return false;
    for(uint32_t i=0;i<levels;++i) {
        const uint64_t pixels=uint64_t(w)*h;
        if(pixels>std::numeric_limits<size_t>::max()/4)return false;
        const uint64_t bytes=pixels*4;
        if(bytes>std::numeric_limits<size_t>::max()-result) return false;
        result+=size_t(bytes); w=w>1?w/2:1; h=h>1?h/2:1;
    }
    return true;
}
inline bool complete_rgba_chain(uint32_t w,uint32_t h,uint32_t levels,const void* data,size_t size) noexcept {
    size_t required=0;
    return data&&rgba_chain_size(w,h,levels,required)&&size>=required;
}
}
