#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace twv::vgl {
constexpr unsigned MaxSparseIndices=2048;
// Called only at cache admission. Keep all referenced record bytes, coalescing
// small gaps to avoid excessive comparisons. The original broad band remains
// the fallback for dense, small, oversized or insufficient-capacity cases.
template<class Span>
unsigned indexed_source_runs(Span* output,unsigned capacity,const uint8_t* array,
                             std::size_t stride,std::size_t width,uint16_t* indices,
                             unsigned count,uint32_t low,uint32_t high) {
    if(!output || !capacity || !array || !indices || !count || high<low || !width)return 0;
    if(width>UINTPTR_MAX-reinterpret_cast<uintptr_t>(array))return 0;
    if(high && stride>(UINTPTR_MAX-reinterpret_cast<uintptr_t>(array)-width)/high)return 0;
    const std::size_t broad=std::size_t(high-low)*stride+width;
    if(broad>2*1024*1024)return 0;
    const auto fallback=[&](){output[0]={array+std::size_t(low)*stride,broad};return 1u;};
    if(count>MaxSparseIndices || broad<4096 || std::size_t(count)*width>broad/2)return fallback();
    std::sort(indices,indices+count);
    if(indices[0]!=low || indices[count-1]!=high)return 0;
    for(std::size_t gap=64;gap<=broad*2;gap*=2){
        unsigned n=0;bool full=false;
        std::size_t begin=std::size_t(indices[0])*stride,end=begin+width;
        for(unsigned i=1;i<count;++i){
            const std::size_t next=std::size_t(indices[i])*stride;
            if(next<=end || next-end<=gap){end=std::max(end,next+width);continue;}
            if(n==capacity){full=true;break;}
            output[n++]={array+begin,end-begin};begin=next;end=next+width;
        }
        if(!full && n<capacity){output[n++]={array+begin,end-begin};return n;}
    }
    return fallback();
}
}
