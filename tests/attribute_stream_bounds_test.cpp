#include "gfx_vgl/attribute_stream.hpp"
#include <dolphin/gx/GXEnum.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <sys/mman.h>
#include <unistd.h>
namespace aurora::gx {
#include "vertex_stage_types.inc"
}
using aurora::gx::AttrConfig;
using twv::vgl::fixed_point_scale;using twv::vgl::normalized_color;
struct V4 {float x,y,z,w;};
// Actual previous scalar/color readers, independent of the stream kernels.
#include "vertex_scalar_color_reference.inc"
using namespace twv::vgl;

void wire(uint8_t* p,uint32_t bits,unsigned width,bool le){
    for(unsigned i=0;i<width;++i)p[le?i:width-1-i]=uint8_t(bits>>(8*i));
}
int main(){
    const size_t page=size_t(sysconf(_SC_PAGESIZE));
    auto* data=static_cast<uint8_t*>(mmap(nullptr,page*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(data!=MAP_FAILED && mprotect(data+page,page,PROT_NONE)==0 && mprotect(data+page*3,page,PROT_NONE)==0);
    uint64_t scalarChecks=0,colorChecks=0;
    auto input=[&](AttrConfig& a,unsigned width){
        // Both the exact source attribute and the wire index end at a guard page.
        auto* source=data+page-width;
        const unsigned indexWidth=a.attrType==GX_INDEX8?1:2;
        auto* packet=a.attrType==GX_DIRECT?source-a.offset:data+3*page-indexWidth-a.offset;
        if(a.attrType!=GX_DIRECT)std::memset(packet+a.offset,0,indexWidth);
        return AttributeStream<AttrConfig>{a,packet,source,uint32_t(width+a.offset)};
    };
    const uint32_t patterns[]={0,1,0x80000000,0x007fffff,0x7f800000,0xff800000,0x7fc12345,0x7fa12345,0xffffffff,0x3f800000,0x12345678};
    for(unsigned kind:{GX_DIRECT,GX_INDEX8,GX_INDEX16})for(bool le:{false,true})
      for(unsigned type=GX_U8;type<=GX_F32;++type)for(unsigned count=1;count<=3;++count)
       for(unsigned frac:{0u,1u,6u,14u,31u})for(unsigned pattern=0;pattern<std::size(patterns);++pattern){
        AttrConfig a{};a.attrType=kind;a.compType=type;a.frac=frac;a.le=le;a.offset=3;
        const unsigned width=type<=GX_S8?1:type<=GX_S16?2:4;a.stride=count*width;
        const auto in=input(a,count*width);const bool effectiveLittle=kind==GX_DIRECT?false:le;
        auto* source=const_cast<uint8_t*>(kind==GX_DIRECT?in.packet+a.offset:in.array);
        for(unsigned c=0;c<count;++c)wire(source+c*width,patterns[(pattern+c)%std::size(patterns)],width,effectiveLittle);
        const float third=pattern%2?1.f:0.f;float expected[3]={0,0,third};
        fetch(a,source,effectiveLittle,count,expected);
        for(unsigned skew=0;skew<16;++skew){
            std::array<uint8_t,64> output;output.fill(0xa5);
            pack_numeric_attribute(in,output.data()+skew+8,20,1,count,third);
            assert(!std::memcmp(output.data()+skew+8,expected,12));
            for(unsigned i=0;i<output.size();++i)if(i<skew+8 || i>=skew+20)assert(output[i]==0xa5);
            ++scalarChecks;
        }
    }
    // Exhaust every two-byte color and every24-bit RGBA6 value. Vary source
    // indexing and endian choice; protected ends still reject accidental overreads.
    for(unsigned type:{GX_RGB565,GX_RGBA4,GX_RGBA6,GX_RGB8,GX_RGBX8,GX_RGBA8}){
        const unsigned width=type==GX_RGB565 || type==GX_RGBA4?2:type==GX_RGBA8?4:3;
        const uint32_t end=width==2?65536:type==GX_RGBA6?1u<<24:4096;
        for(uint32_t bits=0;bits<end;++bits){
            AttrConfig a{};a.attrType=bits%3==0?GX_DIRECT:bits%3==1?GX_INDEX8:GX_INDEX16;
            a.compType=type;a.le=bool(bits&1);a.offset=3;a.stride=width;
            const auto in=input(a,width);const bool le=a.attrType==GX_DIRECT?false:bool(a.le);
            auto* source=const_cast<uint8_t*>(a.attrType==GX_DIRECT?in.packet+a.offset:in.array);
            wire(source,bits*uint32_t(type==GX_RGBA8?0x19271:1),width,le);
            const auto expected=fetch_color(a,source,le);std::array<uint8_t,48> output;output.fill(0x5a);
            const unsigned skew=bits%16;pack_color_attribute(in,output.data()+skew+4,24,1);
            assert(!std::memcmp(output.data()+skew+4,&expected,16));
            for(unsigned i=0;i<output.size();++i)if(i<skew+4 || i>=skew+20)assert(output[i]==0x5a);
            ++colorChecks;
        }
    }
    AttrConfig a{};a.attrType=GX_DIRECT;a.compType=GX_F32;
    pack_numeric_attribute(AttributeStream<AttrConfig>{a,nullptr,nullptr,0},nullptr,12,0,3);
    a.compType=GX_RGBA8;pack_color_attribute(AttributeStream<AttrConfig>{a,nullptr,nullptr,0},nullptr,16,0);
    assert(munmap(data,page*4)==0);
    std::printf("attribute_stream_bounds_test: %llu scalar and %llu color vectors match original readers; all index/endian paths, F32 payloads, source/index guard pages and unaligned output guards\n",static_cast<unsigned long long>(scalarChecks),static_cast<unsigned long long>(colorChecks));
}
