#pragma once
#include "small_packet_cache.hpp"
#include "vertex_contract.hpp"
#include <dolphin/gx/GXEnum.h>

namespace twv::vgl {
// Only complete raw GPU packets whose inputs are entirely in the immediate
// stream. Inspect even unused fields: no indexed array dependency may escape
// the content key. No source byte is read until this predicate succeeds.
template<class Config>
bool small_packet_eligible(const Config& cfg,uint32_t vertices,const void* source,
                           const TwvVitaVertexLayout& layout,bool gpuTexgen) noexcept {
    if(!gpuTexgen || !source || !vertices || vertices>=32 || !cfg.vtxStride ||
       cfg.vtxStride>SmallPacketCache::SourceLimit/vertices || !layout.stride ||
       layout.stride>sizeof(TwvVitaVertex) || layout.stride>SmallPacketCache::OutputLimit/vertices)return false;
    const size_t sourceBytes=size_t(vertices)*cfg.vtxStride;
    if(sourceBytes>UINTPTR_MAX-reinterpret_cast<uintptr_t>(source))return false;
    if(layout.offsets[0]==TwvVertexAttributeAbsent || layout.offsets[12]==TwvVertexAttributeAbsent)return false;
    unsigned packed=0;
    for(unsigned slot=0;slot<TwvVertexAttributeCount;++slot) {
        const auto offset=layout.offsets[slot];
        if(offset==TwvVertexAttributeAbsent)continue;
        const unsigned n=layout.components[slot];
        if(n!=AttributeComponents[slot] && !(slot>=3 && slot<=10 && n==2))return false;
        const unsigned bytes=n*4;
        if(offset>layout.stride || bytes>layout.stride-offset)return false;
        for(unsigned prior=0;prior<slot;++prior) {
            const unsigned other=layout.offsets[prior];
            if(other!=TwvVertexAttributeAbsent && offset<other+layout.components[prior]*4 && other<offset+bytes)return false;
        }
        packed+=bytes;
    }
    if(packed!=layout.stride)return false;
    for(unsigned attr=GX_VA_PNMTXIDX;attr<=GX_VA_TEX7;++attr) {
        const auto& a=cfg.attrs[attr];
        if(a.attrType==GX_NONE)continue;
        if(a.attrType!=GX_DIRECT)return false;
        unsigned bytes=1;
        if(attr==GX_VA_CLR0 || attr==GX_VA_CLR1) {
            switch(a.compType) {
            case GX_RGB565:case GX_RGBA4:bytes=2;break;
            case GX_RGB8:case GX_RGBA6:bytes=3;break;
            case GX_RGBX8:case GX_RGBA8:bytes=4;break;
            default:return false;
            }
        } else if(attr>=GX_VA_POS) {
            if(a.frac>31)return false;
            if(attr==GX_VA_POS) {if(a.cnt!=2 && a.cnt!=3)return false;}
            else if(attr==GX_VA_NRM) {if(a.cnt!=3 || a.nbt3)return false;}
            else if(a.cnt!=1 && a.cnt!=2)return false;
            switch(a.compType) {
            case GX_U8:case GX_S8:bytes=1;break;
            case GX_U16:case GX_S16:bytes=2;break;
            case GX_F32:bytes=4;break;
            default:return false;
            }
            bytes*=a.cnt;
        }
        if(a.offset>cfg.vtxStride || bytes>unsigned(cfg.vtxStride)-a.offset)return false;
    }
    return true;
}
}
