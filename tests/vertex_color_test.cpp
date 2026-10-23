#include "gfx_vgl/color_lut.hpp"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
using twv::vgl::normalized_color;
enum {GX_RGB565,GX_RGB8,GX_RGBX8,GX_RGBA4,GX_RGBA6,GX_RGBA8};
struct AttrConfig {unsigned compType;};
struct V4 {float x,y,z,w;};
#include "vertex_color_functions.inc"
uint32_t bits(float f){uint32_t b;std::memcpy(&b,&f,4);return b;}
uint64_t components=0;
void check(const V4& got,float r,float g,float b,float a) {
    assert(bits(got.x)==bits(r) && bits(got.y)==bits(g) && bits(got.z)==bits(b) && bits(got.w)==bits(a));
    components+=4;
}
float reference(unsigned value,unsigned width){return float(value)/float((1u<<width)-1);}
void packed(unsigned type,uint32_t value,unsigned bytes,bool le) {
    uint8_t encoded[5]{};
    for(unsigned i=0;i<bytes;++i)encoded[1+(le?i:bytes-1-i)]=uint8_t(value>>(i*8));
    const auto got=fetch_color({type},encoded+1,le);
    if(type==GX_RGB565)check(got,reference(value>>11,5),reference((value>>5)&63,6),reference(value&31,5),1.f);
    else if(type==GX_RGBA4)check(got,reference(value>>12,4),reference((value>>8)&15,4),reference((value>>4)&15,4),reference(value&15,4));
    else check(got,reference(value>>18,6),reference((value>>12)&63,6),reference((value>>6)&63,6),reference(value&63,6));
}
int main() {
    // Every packed16-bit and24-bit color, in both endian orders, compared
    // bit-for-bit with the previous mathematical divide, not another lookup.
    for(uint32_t value=0;value<65536;++value)for(unsigned le=0;le<2;++le) {
        packed(GX_RGB565,value,2,le);packed(GX_RGBA4,value,2,le);
    }
    for(uint32_t value=0;value<(1u<<24);++value)for(unsigned le=0;le<2;++le)
        packed(GX_RGBA6,value,3,le);
    // Byte formats ignore endian flags. Exercise all byte levels at every
    // offset and mixed channels, including RGBX's deliberately ignored alpha.
    for(unsigned type:{GX_RGB8,GX_RGBX8,GX_RGBA8})for(uint32_t value=0;value<65536;++value) {
        const uint32_t raw=value*1103515245u+12345u;
        uint8_t bytes[5]={0xAF,uint8_t(raw),uint8_t(raw>>8),uint8_t(raw>>16),uint8_t(raw>>24)};
        for(unsigned le=0;le<2;++le)check(fetch_color({type},bytes+1,le),
            reference(bytes[1],8),reference(bytes[2],8),reference(bytes[3],8),type==GX_RGBA8?reference(bytes[4],8):1.f);
    }
    std::printf("vertex_color_test: %llu exact normalized components, all RGB565/RGBA4/RGBA6 values, mixed byte formats and both endian orders passed\n",static_cast<unsigned long long>(components));
}
