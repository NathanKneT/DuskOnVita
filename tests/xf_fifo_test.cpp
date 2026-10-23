// Real patched five J3D functions vs their compiled original fallback, using
// real Aurora FIFO stores. Includes truncated display lists and FIFO growth.
#include "gfx_vgl/xf_fifo_block.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using f32=float;
using MtxP=float(*)[4];using Mtx3P=float(*)[3];
#define LIKELY [[likely]]
constexpr uint16_t bswap(uint16_t x){return __builtin_bswap16(x);}
constexpr uint32_t bswap(uint32_t x){return __builtin_bswap32(x);}
constexpr uint64_t bswap(uint64_t x){return __builtin_bswap64(x);}
namespace aurora::gx::fifo {
namespace detail {
uint8_t *sBufferData,*sDlBuffer;uint32_t sBufferSize,sBufferCapacity,sDlSize,sDlWritePos;
bool sInDisplayList;
}
unsigned grows=0;
void write_data_grow(const void* data,uint32_t size){
    ++grows;detail::sBufferCapacity=512;
    std::memcpy(detail::sBufferData+detail::sBufferSize,data,size);detail::sBufferSize+=size;
}
#include "fifo_stores.inc"
}
unsigned scalarCalls=0,blockCalls=0;
void GXCmd1u8(u8 x){++scalarCalls;aurora::gx::fifo::write_u8(x);}
void GXCmd1u16(u16 x){++scalarCalls;aurora::gx::fifo::write_u16(x);}
void GXCmd1u32(u32 x){++scalarCalls;aurora::gx::fifo::write_u32(x);}
namespace production {
#include "try_block.inc"
}
bool GXTryCmdBlock(const void* data,u32 n){++blockCalls;return production::GXTryCmdBlock(data,n);}
void J3DFifoWriteXFCmdHdr(u16 address,u8 size){GXCmd1u8(0x10);GXCmd1u16(size-1);GXCmd1u16(address);}
void J3DGXCmd1f32ptr(float* p){u32 v;std::memcpy(&v,p,4);GXCmd1u32(v);}
void J3DGXCmd1f32(float p){J3DGXCmd1f32ptr(&p);}
#define TARGET_VITA 1
namespace original {
#define TWV_VITA_MATRIX_BLOCKS 0
#include "matrix_functions.inc"
#undef TWV_VITA_MATRIX_BLOCKS
}
namespace batched {
#define TWV_VITA_MATRIX_BLOCKS 1
#include "matrix_functions.inc"
#undef TWV_VITA_MATRIX_BLOCKS
}
struct Result {
    std::array<uint8_t,512> bytes;u32 cursor;unsigned scalar,blocks,grows;
};
template<class F> Result run(F f,bool dl,unsigned size,unsigned offset){
    using namespace aurora::gx::fifo;
    Result r{};r.bytes.fill(0xa5);
    detail::sBufferData=detail::sDlBuffer=r.bytes.data();
    detail::sInDisplayList=dl;detail::sBufferSize=detail::sDlWritePos=offset;
    detail::sBufferCapacity=detail::sDlSize=size;scalarCalls=blockCalls=grows=0;
    f();r.cursor=dl?detail::sDlWritePos:detail::sBufferSize;
    r.scalar=scalarCalls;r.blocks=blockCalls;r.grows=grows;return r;
}
template<class Old,class New> void compare(Old old,New fast,unsigned words){
    for(unsigned size=0;size<100;++size)for(unsigned offset=0;offset<=std::min(size,8u);++offset){
        const auto a=run(old,true,size,offset),b=run(fast,true,size,offset);
        assert(a.cursor==b.cursor && a.bytes==b.bytes);
        assert(a.scalar==words+3 && b.blocks==1);
        assert(b.scalar==(size-offset>=5+4*words?0:words+3));
    }
    for(unsigned size:{0u,8u,512u}){
        auto a=run(old,false,size,0),b=run(fast,false,size,0);
        assert(a.cursor==b.cursor && a.bytes==b.bytes);
        assert(b.scalar==0 && b.blocks==1 && b.grows==(size<5+4*words));
    }
}
int main(){
    u32 seed=0x319af47d;
    for(unsigned sample=0;sample<128;++sample){
        float matrix[3][4],normal[3][3];
        for(unsigned i=0;i<12;++i){
            seed=seed*1664525u+1013904223u;
            const u32 bits=sample<8?std::array<u32,8>{0,0x80000000u,0x7f800000u,0xff800000u,
                0x7fc12345u,0x7fa12345u,1,0xffffffffu}[sample]:seed;
            std::memcpy(&matrix[i/4][i%4],&bits,4);
            if(i<9)std::memcpy(&normal[i/3][i%3],&bits,4);
        }
        const u32 id=sample%2?sample:0xfffffff0u+sample;
#define CHECK(Name,Matrix,Words) compare([&]{original::Name(Matrix,id);},[&]{batched::Name(Matrix,id);},Words)
        CHECK(J3DFifoLoadPosMtxImm,matrix,12);
        CHECK(J3DFifoLoadNrmMtxImm,matrix,9);
        CHECK(J3DFifoLoadNrmMtxImm3x3,normal,9);
        CHECK(J3DFifoLoadNrmMtxToTexMtx,matrix,12);
        CHECK(J3DFifoLoadNrmMtxToTexMtx3x3,normal,12);
#undef CHECK
    }
    // Independent known-endian vector, beyond comparing two code paths.
    twv::vgl::XfFifoBlock<9> b(0x1234);b.word(0,0x89abcdef);
    const uint8_t expected[]={0x10,0,8,0x12,0x34,0x89,0xab,0xcd,0xef};
    assert(std::memcmp(expected,b.bytes,sizeof expected)==0);
    std::puts("xf_fifo_test: five production matrices preserve bits, FIFO growth, all short-list cursors/canaries");
}
