// Compile the real production constructor, dependency plan, address resolver
// and both decoders, with actual GX enums and Aurora attribute structs.
#include "gfx_vgl/fixed_scale.hpp"
#include "gfx_vgl/color_lut.hpp"
#include "gfx_vgl/vertex_contract.hpp"
#include "gfx_vgl/attribute_stream.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include <dolphin/gx/GXEnum.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace aurora::gx {
constexpr unsigned MaxPnMtx=10;
#include "vertex_stage_types.inc"
struct ShaderConfig {
    uint8_t vtxStride=0;
    std::array<AttrConfig,GX_VA_TEX7+1> attrs;
    std::array<ColorChannelConfig,4> colorChannels;
    std::array<TcgConfig,8> tcgs;
};
struct DrawData {
    uint32_t vtxCount=0;
    struct {
        uintptr_t vtxStart=0;
        std::array<uintptr_t,12> arrayStart{};
        uint32_t currentPnMtx=0;
    } immediateData;
};
}
namespace twv::vgl {
struct Pipeline {
    struct {aurora::gx::ShaderConfig shaderConfig;} config;
    struct {std::bitset<8> sampledTexCoords;} info;
};
using aurora::gx::AttrConfig;
#include "vertex_stage_decode.inc"
}
using namespace twv::vgl;

uint32_t random_state=0x34a144a4;
uint32_t random_value() {
    random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;
    return random_state;
}
uint32_t bits(float f) {uint32_t b;std::memcpy(&b,&f,4);return b;}
void same3(const V3& a,const V3& b) {assert(bits(a.x)==bits(b.x) && bits(a.y)==bits(b.y) && bits(a.z)==bits(b.z));}
void same(const Decoded& a,const Decoded& b) {
    assert(a.pnmtx==b.pnmtx);same3(a.pos,b.pos);same3(a.nrm,b.nrm);same3(a.binrm,b.binrm);same3(a.tangent,b.tangent);
    for(unsigned c=0;c<2;++c) {
        assert(bits(a.clr[c].x)==bits(b.clr[c].x) && bits(a.clr[c].y)==bits(b.clr[c].y));
        assert(bits(a.clr[c].z)==bits(b.clr[c].z) && bits(a.clr[c].w)==bits(b.clr[c].w));
    }
    for(unsigned c=0;c<8;++c) {
        assert(bits(a.tex[c][0])==bits(b.tex[c][0]) && bits(a.tex[c][1])==bits(b.tex[c][1]));
        assert(a.texmtx[c]==b.texmtx[c]);
    }
}
unsigned width(const AttrConfig& a,unsigned attr) {
    if(attr<GX_VA_POS)return 1;
    if(attr==GX_VA_CLR0 || attr==GX_VA_CLR1)
        return a.compType==GX_RGB565 || a.compType==GX_RGBA4?2:a.compType==GX_RGB8 || a.compType==GX_RGBA6?3:4;
    const unsigned scalar=a.compType==GX_U8 || a.compType==GX_S8?1:a.compType==GX_U16 || a.compType==GX_S16?2:4;
    return a.cnt*scalar;
}
int main() {
    uint64_t vertices=0,streamedVertices=0;bool directSeen=false,index8Seen=false,index16Seen=false,nbtSeen=false,nbt3Seen=false;
    bool compactDirect=false,compactIndex8=false,compactIndex16=false;
    bool compactLE=false,compactBE=false,compactDefault=false,compactSlot9=false;
    bool compactNaN=false,compactNegativeZero=false;
    for(unsigned trial=0;trial<2048;++trial) {
        Pipeline pipeline;auto& cfg=pipeline.config.shaderConfig;
        for(auto& channel:cfg.colorChannels) {
            channel.matSrc=random_value()%3?GX_SRC_VTX:GX_SRC_REG;
            channel.ambSrc=random_value()%2?GX_SRC_VTX:GX_SRC_REG;
            channel.lightingEnabled=bool(random_value()%2);
        }
        for(unsigned tc=0;tc<8;++tc) {
            cfg.tcgs[tc].src=GXTexGenSrc(GX_TG_TEX0+(trial%4==0?random_value()%8:tc));
            if(random_value()%4)pipeline.info.sampledTexCoords.set(tc);
        }
        const bool nbt=trial%3==0;
        if(nbt) {pipeline.info.sampledTexCoords.set(0);cfg.tcgs[0].type=GX_TG_BUMP0;cfg.tcgs[0].embossSrc=1;}
        unsigned stride=0;
        for(unsigned attr=GX_VA_PNMTXIDX;attr<=GX_VA_TEX7;++attr) {
            auto& a=cfg.attrs[attr];
            if(attr<GX_VA_POS)a.attrType=random_value()%4?GX_DIRECT:GX_NONE;
            else a.attrType=uint8_t(random_value()%4);
            a.cnt=uint8_t(attr==GX_VA_POS?(random_value()%2?2:3):attr==GX_VA_NRM?(nbt?9:3):attr==GX_VA_CLR0 || attr==GX_VA_CLR1?4:random_value()%2+1);
            a.compType=uint8_t(random_value()%((attr==GX_VA_CLR0 || attr==GX_VA_CLR1)?6:5));
            a.frac=uint8_t(random_value()%32);a.le=bool(random_value()%2);
            a.nbt3=attr==GX_VA_NRM && nbt && random_value()%2;
            a.offset=uint8_t(stride);a.stride=uint8_t(width(a,attr)+random_value()%4);
            if(a.attrType!=GX_NONE) {
                const unsigned indices=a.nbt3?3:1;
                stride+=a.attrType==GX_DIRECT?width(a,attr):a.attrType==GX_INDEX8?indices:indices*2;
                directSeen|=a.attrType==GX_DIRECT;index8Seen|=a.attrType==GX_INDEX8;index16Seen|=a.attrType==GX_INDEX16;
                nbtSeen|=attr==GX_VA_NRM && nbt;nbt3Seen|=a.nbt3;
            }
        }
        assert(stride<=255);cfg.vtxStride=uint8_t(std::max(stride,1u));
        std::array<std::vector<uint8_t>,12> arrays;std::array<unsigned,12> skips{};
        aurora::gx::DrawData draw;
        for(unsigned i=0;i<12;++i) {
            const auto& a=cfg.attrs[GX_VA_POS+i];skips[i]=random_value()%16;
            arrays[i].resize(skips[i]+261*a.stride+width(a,GX_VA_POS+i)+16);
            for(auto& byte:arrays[i])byte=uint8_t(random_value());
            draw.immediateData.arrayStart[i]=reinterpret_cast<uintptr_t>(arrays[i].data()+skips[i]);
        }
        const unsigned skip=random_value()%16;std::vector<uint8_t> raw(skip+size_t(cfg.vtxStride)*24);
        for(auto& byte:raw)byte=uint8_t(random_value());
        for(unsigned v=0;v<24;++v)for(unsigned attr=GX_VA_PNMTXIDX;attr<=GX_VA_TEX7;++attr) {
            const auto& a=cfg.attrs[attr];auto* p=raw.data()+skip+size_t(v)*cfg.vtxStride+a.offset;
            if(a.attrType==GX_DIRECT && attr==GX_VA_PNMTXIDX)*p=uint8_t(random_value()%30);
            if(a.attrType==GX_INDEX8 || a.attrType==GX_INDEX16)for(unsigned i=0;i<(a.nbt3?3u:1u);++i) {
                if(a.attrType==GX_INDEX8)p[i]=uint8_t(random_value());
                else {const unsigned index=random_value()%260;p[i*2]=uint8_t(index>>8);p[i*2+1]=uint8_t(index);}
            }
        }
        // Force exact F32 payloads through the real address resolver and ST2
        // packet writer; ordinary random floats do not guarantee NaN coverage.
        for(unsigned attr=GX_VA_TEX0;attr<=GX_VA_TEX7;++attr) {
            const auto& a=cfg.attrs[attr];
            if(a.attrType==GX_NONE || a.compType!=GX_F32)continue;
            auto* p=raw.data()+skip+a.offset;
            if(a.attrType!=GX_DIRECT) {
                const unsigned index=a.attrType==GX_INDEX8?*p:(unsigned(p[0])<<8)|p[1];
                const unsigned slot=attr-GX_VA_POS;p=arrays[slot].data()+skips[slot]+size_t(index)*a.stride;
            }
            const bool le=a.attrType!=GX_DIRECT && a.le;
            for(unsigned c=0;c<a.cnt;++c) {
                const uint32_t payload=c?0x80000000u:0x7fa12345u;
                for(unsigned b=0;b<4;++b)p[c*4+(le?b:3-b)]=uint8_t(payload>>(b*8));
            }
        }
        draw.immediateData.vtxStart=reinterpret_cast<uintptr_t>(raw.data()+skip);
        draw.vtxCount=24;
        VertexStage stage(pipeline,draw);Decoded optimized{},reference{};
        for(unsigned v=0;v<24;++v) {
            // The plan must not snapshot live array contents or a default PN.
            draw.immediateData.currentPnMtx=(trial+v)%10;
            const unsigned slot=v%12;arrays[slot][skips[slot]+v]^=1;
            stage.decode(v,optimized);stage.decode_reference(v,reference);same(optimized,reference);++vertices;
        }
        for(bool rawST: {false,true}) {
        const auto layout=compact_vertex_layout(uint32_t(pipeline.info.sampledTexCoords.to_ulong()),random_value()%4,bool(random_value()%2),rawST);
        if(stage.can_pack_raw_streams(layout)) {
            streamedVertices+=draw.vtxCount;
            const unsigned guard=16+trial%16;std::vector<uint8_t> packed(guard+size_t(layout.stride)*draw.vtxCount+32,0xa5);
            unsigned slots=0;assert(stage.pack_raw_streams(layout,packed.data()+guard,slots));
            assert(stage.validate_raw_streams(layout,packed.data()+guard,slots));
            if(rawST) {
                compactSlot9|=slots==10;
                for(unsigned tc=0;tc<8;++tc)if(layout.offsets[3+tc]!=TwvVertexAttributeAbsent) {
                    const auto& a=cfg.attrs[unsigned(GX_VA_TEX0)+unsigned(cfg.tcgs[tc].src)-unsigned(GX_TG_TEX0)];
                    compactDirect|=a.attrType==GX_DIRECT;compactIndex8|=a.attrType==GX_INDEX8;
                    compactIndex16|=a.attrType==GX_INDEX16;compactDefault|=a.attrType==GX_NONE;
                    if(a.attrType!=GX_NONE){compactLE|=a.le;compactBE|=!a.le;}
                    if(a.attrType!=GX_NONE && a.compType==GX_F32) {
                        uint32_t payload[2];std::memcpy(payload,packed.data()+guard+layout.offsets[3+tc],sizeof payload);
                        compactNaN|=payload[0]==0x7fa12345u;
                        compactNegativeZero|=payload[1]==0x80000000u;
                    }
                    if(a.attrType==GX_NONE)for(unsigned v=0;v<draw.vtxCount;++v) {
                        float value[2];std::memcpy(value,packed.data()+guard+size_t(v)*layout.stride+layout.offsets[3+tc],sizeof value);
                        assert(value[0]==0.f && value[1]==0.f);
                    }
                }
            }
            assert(std::all_of(packed.begin(),packed.begin()+guard,[](auto b){return b==0xa5;}));
            assert(std::all_of(packed.end()-32,packed.end(),[](auto b){return b==0xa5;}));
            // Negative checks at changing offsets throughout the output packet.
            for(size_t byte=0;byte<size_t(layout.stride)*draw.vtxCount;byte+=1+layout.stride/3) {
                packed[guard+byte]^=1;assert(!stage.validate_raw_streams(layout,packed.data()+guard,slots));packed[guard+byte]^=1;
            }
            assert(!stage.validate_raw_streams(layout,packed.data()+guard,slots==10?9:slots+1));
            if(cfg.attrs[GX_VA_PNMTXIDX].attrType==GX_NONE) {
                const auto before=draw.immediateData.currentPnMtx;draw.immediateData.currentPnMtx=10;
                assert(!stage.pack_raw_streams(layout,packed.data()+guard,slots));draw.immediateData.currentPnMtx=before;
            } else {
                auto& pn=raw[skip+cfg.attrs[GX_VA_PNMTXIDX].offset];const auto before=pn;pn=30;
                assert(!stage.pack_raw_streams(layout,packed.data()+guard,slots));pn=before;
            }
            draw.vtxCount=0;assert(stage.pack_raw_streams(layout,nullptr,slots) && slots==1);draw.vtxCount=24;
        }
        }
    }
    assert(directSeen && index8Seen && index16Seen && nbtSeen && nbt3Seen);
    assert(compactDirect && compactIndex8 && compactIndex16 && compactLE && compactBE && compactDefault && compactSlot9);
    assert(compactNaN && compactNegativeZero);
    assert(streamedVertices>24000);
    std::printf("vertex_attribute_stream: %llu packed vertices match the independent old decoder, including alias texcoords, missing/live sources, PN extents, invalid PN and negative-byte checks\n",static_cast<unsigned long long>(streamedVertices));
    std::printf("vertex_stage_decode_test: %llu production planned/original vertices, all fields, missing attributes, direct/index8/index16, endian, NBT/NBT3, unaligned arrays and live inputs passed\n",static_cast<unsigned long long>(vertices));
    return 0;
}
