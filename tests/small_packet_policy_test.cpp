#include "gfx_vgl/small_packet_policy.hpp"
#include <array>
#include <cassert>
#include <sys/mman.h>
#include <unistd.h>
using namespace twv::vgl;
struct Attr {uint8_t attrType=GX_NONE,compType=GX_F32,cnt=3,offset=0,stride=0,frac=0;bool nbt3=false;};
struct Config {uint8_t vtxStride=32;std::array<Attr,GX_VA_TEX7+1> attrs{};};
int main() {
    Config cfg;cfg.attrs[GX_VA_POS]={GX_DIRECT,GX_F32,3,0};
    cfg.attrs[GX_VA_CLR0]={GX_DIRECT,GX_RGBA8,4,12};
    cfg.attrs[GX_VA_TEX0]={GX_DIRECT,GX_F32,2,16};
    std::array<uint8_t,512> source{};
    auto layout=compact_vertex_layout(1,1,false,true);
    auto valid=[&]{return small_packet_eligible(cfg,4,source.data(),layout,true);};
    assert(valid());
    for(unsigned attr=GX_VA_PNMTXIDX;attr<=GX_VA_TEX7;++attr) {
        const auto original=cfg.attrs[attr];
        for(unsigned kind:{unsigned(GX_INDEX8),unsigned(GX_INDEX16),99u}) {
            cfg.attrs[attr].attrType=uint8_t(kind);assert(!valid());
        }
        cfg.attrs[attr]=original;
    }
    for(unsigned type:{unsigned(GX_U8),unsigned(GX_S8),unsigned(GX_U16),unsigned(GX_S16),unsigned(GX_F32)}) {
        cfg.attrs[GX_VA_POS].compType=uint8_t(type);assert(valid());
    }
    cfg.attrs[GX_VA_POS].compType=99;assert(!valid());cfg.attrs[GX_VA_POS].compType=GX_F32;
    cfg.attrs[GX_VA_POS].cnt=4;assert(!valid());cfg.attrs[GX_VA_POS].cnt=3;
    cfg.attrs[GX_VA_POS].frac=32;assert(!valid());cfg.attrs[GX_VA_POS].frac=0;
    for(unsigned format=GX_RGB565;format<=GX_RGBA8;++format) {
        auto& color=cfg.attrs[GX_VA_CLR0];color.compType=uint8_t(format);
        const unsigned bytes=format==GX_RGB565 || format==GX_RGBA4?2:format==GX_RGB8 || format==GX_RGBA6?3:4;
        color.offset=uint8_t(32-bytes);assert(valid());++color.offset;assert(!valid());
    }
    cfg.attrs[GX_VA_CLR0]={GX_DIRECT,GX_RGBA8,4,12};
    cfg.attrs[GX_VA_NRM]={GX_DIRECT,GX_U8,9,20};assert(!valid());
    cfg.attrs[GX_VA_NRM].cnt=3;assert(valid());
    cfg.attrs[GX_VA_NRM].nbt3=true;assert(!valid());cfg.attrs[GX_VA_NRM]={};
    cfg.attrs[GX_VA_TEX0].offset=25;assert(!valid());cfg.attrs[GX_VA_TEX0].offset=24;assert(valid());
    cfg.attrs[GX_VA_TEX0].cnt=3;assert(!valid());cfg.attrs[GX_VA_TEX0].cnt=2;
    const auto original=layout;
    layout.offsets[3]=layout.offsets[0];assert(!valid());layout=original;
    ++layout.stride;assert(!valid());layout=original;
    layout.components[3]=4;assert(!valid());layout=original;
    assert(!small_packet_eligible(cfg,0,source.data(),layout,true));
    assert(!small_packet_eligible(cfg,32,source.data(),layout,true));
    assert(!small_packet_eligible(cfg,4,nullptr,layout,true));
    assert(!small_packet_eligible(cfg,4,source.data(),layout,false));
    assert(!small_packet_eligible(cfg,4,reinterpret_cast<void*>(UINTPTR_MAX-1),layout,true));
    // Policy rejects before hashing/reading, even when the source is inaccessible.
    const size_t page=size_t(sysconf(_SC_PAGESIZE));
    auto* inaccessible=mmap(nullptr,page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(inaccessible!=MAP_FAILED);
    cfg.attrs[GX_VA_TEX7].attrType=GX_INDEX16;
    assert(!small_packet_eligible(cfg,4,inaccessible,layout,true));
    assert(munmap(inaccessible,page)==0);
}
