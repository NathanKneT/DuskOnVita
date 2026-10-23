// Exercise the actual production fetch function extracted by host_tests.sh.
#include "gfx_vgl/fixed_scale.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
using twv::vgl::fixed_point_scale;
enum {GX_U8,GX_S8,GX_U16,GX_S16,GX_F32};
struct AttrConfig {unsigned frac,compType;};
#include "vertex_fetch_function.inc"
uint32_t bits(float f){uint32_t b;std::memcpy(&b,&f,4);return b;}
void put(uint8_t* dst,uint32_t value,unsigned width,bool le) {
    for(unsigned i=0;i<width;++i)dst[le?i:width-1-i]=uint8_t(value>>(i*8));
}
int main() {
    uint64_t components=0;
    for(unsigned frac=0;frac<32;++frac) {
        const float referenceScale=1.f/static_cast<float>(uint32_t(1)<<frac);
        assert(bits(fixed_point_scale(frac))==bits(referenceScale));
        for(unsigned type=GX_U8;type<=GX_S16;++type) {
            const unsigned width=type<2?1:2,limit=width==1?256:65536;
            PlannedFetch plans[2][3];
            for(unsigned le=0;le<2;++le)for(unsigned n=1;n<=3;++n)plans[le][n-1]=plan_fetch({frac,type},le,n);
            for(unsigned value=0;value<limit;++value)for(unsigned le=0;le<2;++le) {
                // Rotate component counts while exhaustively covering every
                // signed/unsigned value, fractional shift and byte order.
                const unsigned count=value%3+1;uint8_t encoded[13]{};
                std::array<float,5> output;output.fill(-9999.25f);
                for(unsigned c=0;c<count;++c)put(encoded+1+c*width,(value+c*17)&(limit-1),width,le);
                fetch({frac,type},encoded+1,le,count,output.data()+1);
                std::array<float,5> planned;planned.fill(-9999.25f);
                plans[le][count-1].read(encoded+1,planned.data()+1);
                assert(output[0]==-9999.25f && output[count+1]==-9999.25f);
                assert(planned[0]==-9999.25f && planned[count+1]==-9999.25f);
                for(unsigned c=0;c<count;++c) {
                    const uint32_t raw=(value+c*17)&(limit-1);
                    const int integer=type==GX_S8?int(int8_t(raw)):type==GX_S16?int(int16_t(raw)):int(raw);
                    assert(bits(output[c+1])==bits(float(integer)*referenceScale));
                    assert(bits(planned[c+1])==bits(output[c+1]));++components;
                }
            }
        }
    }
    // F32 must preserve all source bits, including signed zero, subnormals,
    // infinity and NaN payloads. Scaling is irrelevant to this source format.
    const uint32_t patterns[]={0,0x80000000,0x3f800000,0x7f800000,0xff800000,
        0x7fc01234,0x7f800001,1,0x007fffff,0x7f7fffff,0x3f9e0651};
    for(unsigned frac=0;frac<32;++frac)for(unsigned le=0;le<2;++le)
      for(unsigned count=1;count<=3;++count)for(unsigned begin=0;begin<std::size(patterns);++begin) {
        uint8_t encoded[13]{};float output[5]={-9999,-9999,-9999,-9999,-9999};
        for(unsigned c=0;c<count;++c)put(encoded+1+c*4,patterns[(begin+c)%std::size(patterns)],4,le);
        fetch({frac,GX_F32},encoded+1,le,count,output+1);
        float planned[5]={-9999,-9999,-9999,-9999,-9999};
        plan_fetch({frac,GX_F32},le,count).read(encoded+1,planned+1);
        assert(output[0]==-9999 && output[count+1]==-9999);
        assert(planned[0]==-9999 && planned[count+1]==-9999);
        for(unsigned c=0;c<count;++c) {
            assert(bits(output[c+1])==patterns[(begin+c)%std::size(patterns)]);
            assert(bits(planned[c+1])==bits(output[c+1]));
        }
    }
    float untouched=73.f;fetch({31,GX_U16},nullptr,true,0,&untouched);assert(untouched==73.f);
    // Malformed shifts are outside GX's five-bit contract. The fast helper
    // bounds them without C++ shift UB; never run the legacy undefined shift.
    assert(std::isinf(fixed_point_scale(32)) && std::isinf(fixed_point_scale(255)));
    assert(!plan_fetch({0,GX_F32},false,0).function && !plan_fetch({0,GX_F32},true,4).function);
    const size_t page=size_t(sysconf(_SC_PAGESIZE));
    auto* memory=static_cast<uint8_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(memory!=MAP_FAILED && mprotect(memory+page,page,PROT_NONE)==0);
    for(unsigned type=GX_U8;type<=GX_F32;++type)for(unsigned n=1;n<=3;++n)for(bool le:{false,true}) {
        const unsigned width=type<2?1:type<4?2:4;auto* p=memory+page-n*width;
        std::memset(p,0x3e,n*width);float got[3],reference[3];
        fetch({7,type},p,le,n,reference);plan_fetch({7,type},le,n).read(p,got);
        for(unsigned i=0;i<n;++i)assert(bits(got[i])==bits(reference[i]));
    }
    assert(munmap(memory,page*2)==0);
    std::printf("vertex_fetch_test: %llu planned/original exact integer components, both byte orders, unaligned inputs, protected-page ends, output guards and F32 payloads passed\n",static_cast<unsigned long long>(components));
}
