#pragma once
// CPU-only conservative X/Y clip rejection. No near/far or occlusion test.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace twv::vgl {
struct PositionBounds {
    float low[3]{},high[3]{};
    bool valid=false;
};
inline PositionBounds position_bounds(const void* vertices,std::size_t bytes,
                                     std::size_t count,unsigned stride,unsigned offset,unsigned pnSlots) {
    PositionBounds result;
    if(!vertices || !count || pnSlots!=1 || stride<12 || offset>stride-12 || count>bytes/stride)return result;
    const auto* data=static_cast<const uint8_t*>(vertices);
    for(unsigned a=0;a<3;++a){result.low[a]=std::numeric_limits<float>::infinity();result.high[a]=-result.low[a];}
    for(std::size_t i=0;i<count;++i)for(unsigned a=0;a<3;++a){
        float value;std::memcpy(&value,data+i*stride+offset+a*4,4);
        if(!std::isfinite(value) || std::abs(value)>1.e12f)return {};
        result.low[a]=std::min(result.low[a],value);result.high[a]=std::max(result.high[a],value);
    }
    result.valid=true;return result;
}
namespace clip_detail {
struct Range {double low,high;};
// Inflate BOTH stages separately. The relative envelope is deliberately much
// larger than float32 dot/FMA roundoff; the absolute envelope covers tiny/FTZ
// inputs. Cancellation uses sum-of-magnitudes, not the small rounded result.
// Inputs/intermediates outside the bounded finite domain always stay visible.
template<class Row>
bool transform_range(const Row& row,const Range* input,Range& output) {
    double low=row[3],high=row[3],magnitude=std::abs(double(row[3]));
    if(!std::isfinite(low) || magnitude>1.e8)return false;
    for(unsigned a=0;a<3;++a){
        const double c=row[a];if(!std::isfinite(c) || std::abs(c)>1.e8)return false;
        const double x=c*input[a].low,y=c*input[a].high;
        low+=std::min(x,y);high+=std::max(x,y);magnitude+=std::max(std::abs(x),std::abs(y));
    }
    if(!std::isfinite(magnitude) || magnitude>1.e25)return false;
    const double margin=magnitude*1.e-4+1.e-5;
    output={low-margin,high+margin};return true;
}
}
// 0=draw;1=left;2=right;3=bottom;4=top. Only slot0 packets are admitted by
// position_bounds. Matrices are the LIVE PN and projection used by the Cg
// shader in this presentation, not matrices retained with the geometry.
template<class Pn,class Projection>
unsigned offscreen_plane(const PositionBounds& bounds,const Pn& pn,const Projection& projection) {
    if(!bounds.valid)return 0;
    clip_detail::Range object[3],view[3],clip[3];
    for(unsigned a=0;a<3;++a){
        if(!std::isfinite(bounds.low[a]) || !std::isfinite(bounds.high[a]) || bounds.low[a]>bounds.high[a] ||
           std::abs(bounds.low[a])>1.e12f || std::abs(bounds.high[a])>1.e12f)return 0;
        object[a]={bounds.low[a],bounds.high[a]};
    }
    if(!clip_detail::transform_range(pn.m0,object,view[0]) ||
       !clip_detail::transform_range(pn.m1,object,view[1]) ||
       !clip_detail::transform_range(pn.m2,object,view[2]) ||
       !clip_detail::transform_range(projection.m0,view,clip[0]) ||
       !clip_detail::transform_range(projection.m1,view,clip[1]) ||
       !clip_detail::transform_range(projection.m3,view,clip[2]))return 0;
    if(clip[2].high+clip[0].high < -1.e-5)return 1;
    if(clip[2].high-clip[0].low  < -1.e-5)return 2;
    if(clip[2].high+clip[1].high < -1.e-5)return 3;
    if(clip[2].high-clip[1].low  < -1.e-5)return 4;
    return 0;
}
// Independent sampled oracle scans every packed position and checks its actual
// two-stage float32 transform against the chosen plane. Does not reuse bounds
// or interval arithmetic. A failure disables rejection for the whole boot.
template<class Pn,class Projection>
bool verify_offscreen_positions(const void* vertices,std::size_t bytes,std::size_t count,
                                unsigned stride,unsigned offset,unsigned plane,const Pn& pn,const Projection& projection) {
    if(!vertices || !count || !plane || plane>4 || stride<12 || offset>stride-12 || count>bytes/stride)return false;
    const auto* data=static_cast<const uint8_t*>(vertices);
    const auto dot=[](const auto& row,const float* p){return ((row[0]*p[0]+row[1]*p[1])+row[2]*p[2])+row[3];};
    for(std::size_t i=0;i<count;++i){
        float p[3];std::memcpy(p,data+i*stride+offset,12);
        const float view[]={dot(pn.m0,p),dot(pn.m1,p),dot(pn.m2,p)};
        const float x=dot(projection.m0,view),y=dot(projection.m1,view),w=dot(projection.m3,view);
        if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w))return false;
        const double side=plane==1?double(w)+x:plane==2?double(w)-x:plane==3?double(w)+y:double(w)-y;
        if(!(side<0))return false;
    }
    return true;
}
}
