#include "gfx_vgl/clip_bounds.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>
struct Matrix {std::array<float,4> m0{1,0,0,0},m1{0,1,0,0},m2{0,0,1,0},m3{0,0,0,1};};
uint32_t randomState=0x51c011;
float randomValue(float scale){randomState^=randomState<<13;randomState^=randomState>>17;randomState^=randomState<<5;return (float(randomState&65535)/32767.5f-1)*scale;}
int main(){
    using namespace twv::vgl;
    Matrix identity;std::array<float,24> vertices{};
    for(unsigned i=0;i<8;++i){vertices[i*3]=-4+float(i&1);vertices[i*3+1]=float((i>>1)&1)*.1f;}
    auto bounds=position_bounds(vertices.data(),sizeof vertices,8,12,0,1);
    assert(bounds.valid && offscreen_plane(bounds,identity,identity)==1);
    assert(verify_offscreen_positions(vertices.data(),sizeof vertices,8,12,0,1,identity,identity));
    assert(!verify_offscreen_positions(vertices.data(),sizeof vertices,8,12,0,2,identity,identity));
    for(unsigned plane=1;plane<=4;++plane){
        auto b=bounds;for(unsigned a=0;a<3;++a)b.low[a]=b.high[a]=0;
        const unsigned axis=plane<=2?0:1;b.low[axis]=b.high[axis]=(plane&1)?-3:3;
        assert(offscreen_plane(b,identity,identity)==plane);
        b.low[axis]=-.5f;b.high[axis]=.5f;assert(!offscreen_plane(b,identity,identity));
        b.low[axis]=b.high[axis]=(plane&1)?-1:1;assert(!offscreen_plane(b,identity,identity));
    }
    assert(!position_bounds(nullptr,sizeof vertices,8,12,0,1).valid);
    assert(!position_bounds(vertices.data(),sizeof vertices-1,8,12,0,1).valid);
    assert(!position_bounds(vertices.data(),sizeof vertices,8,12,1,1).valid);
    assert(!position_bounds(vertices.data(),sizeof vertices,8,12,0,2).valid);
    for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),1.e20f}){
        vertices[0]=bad;assert(!position_bounds(vertices.data(),sizeof vertices,8,12,0,1).valid);
        Matrix m;m.m0[0]=bad;assert(!offscreen_plane(bounds,m,identity));assert(!offscreen_plane(bounds,identity,m));
    }
    // Positive controls near cancellation, near w=0, tiny/FTZ-scale coordinates,
    // negative scales, perspective and changed live matrices. Visible or unknown
    // must stay visible; false negatives only cost extra original rendering.
    Matrix moved;moved.m0[3]=4;assert(!offscreen_plane(bounds,moved,identity));
    PositionBounds tiny{};tiny.valid=true;
    for(unsigned a=0;a<3;++a){tiny.low[a]=-1.e-40f;tiny.high[a]=1.e-40f;}
    assert(!offscreen_plane(tiny,identity,identity));
    uint64_t rejected=0,checks=0;
    for(unsigned trial=0;trial<120000;++trial){
        Matrix pn,projection;
        for(auto* row:{&pn.m0,&pn.m1,&pn.m2})for(float& value:*row)value=randomValue(4);
        for(auto* row:{&projection.m0,&projection.m1,&projection.m3})for(float& value:*row)value=randomValue(2);
        pn.m0[3]*=5;pn.m1[3]*=5;pn.m2[3]*=5;
        if(trial%3==0){projection=Matrix{};projection.m3={0,0,-1,0};}
        const float shift[3]={randomValue(20),randomValue(20),randomValue(20)};
        for(unsigned i=0;i<8;++i)for(unsigned a=0;a<3;++a)vertices[i*3+a]=shift[a]+randomValue(2);
        const auto b=position_bounds(vertices.data(),sizeof vertices,8,12,0,1);
        const auto plane=offscreen_plane(b,pn,projection);
        if(!plane)continue;
        ++rejected;assert(verify_offscreen_positions(vertices.data(),sizeof vertices,8,12,0,plane,pn,projection));
        // Separate double/FMA path checks all eight box corners and their convex
        // interior points, rather than duplicating interval implementation.
        for(unsigned i=0;i<16;++i){
            double p[3];for(unsigned a=0;a<3;++a)p[a]=i<8?((i&(1u<<a))?b.low[a]:b.high[a]):double(b.low[a])*.25+double(b.high[a])*.75;
            const auto dot=[](const auto& row,const auto* v){return std::fma(double(row[0]),v[0],std::fma(double(row[1]),v[1],std::fma(double(row[2]),v[2],double(row[3]))));};
            const double view[]={dot(pn.m0,p),dot(pn.m1,p),dot(pn.m2,p)};
            const double x=dot(projection.m0,view),y=dot(projection.m1,view),w=dot(projection.m3,view);
            assert((plane==1?w+x:plane==2?w-x:plane==3?w+y:w-y)<0);++checks;
        }
    }
    assert(rejected>1000);
    // Metadata follows the same source-byte validity and lifetime as vertices.
    GeometryCache cache(1024);GeometryKey key{};uint8_t source[8]{};
    SourceSpan span{source,sizeof source};vertices.fill(0);
    assert(cache.store(1,key,&span,1,vertices.data(),sizeof vertices,1,bounds));
    auto hit=cache.lookup(1,key);assert(hit.data && hit.bounds.valid);
    source[0]=1;hit=cache.lookup(1,key);assert(!hit.data && !hit.bounds.valid);
    cache.clear();assert(cache.store(1,key,&span,1,vertices.data(),sizeof vertices,2,bounds));
    assert(!cache.lookup(1,key).bounds.valid);cache.clear();assert(!cache.bytes());
    std::printf("clip_bounds_test:120000 random transforms,%llu conservative rejections,%llu independent box checks; float32 oracle, FMA/double corners, boundaries/nonfinite/FTZ/domain and cache lifetime passed\n",(unsigned long long)rejected,(unsigned long long)checks);
}
