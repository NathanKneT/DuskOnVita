#pragma once
// Per-frame digest of everything the front end hands to the GPU backend at the
// twv_vgl_* interface (G0, docs/ARCHITECTURE.md). It hashes content, never handles or
// addresses, so two backends (or two runs) fed the same draws give the same value:
// - a program is the hash of its two Cg sources;
// - a texture is the hash of its size and every byte uploaded so far, plus the
//   object sampler last set on it;
// - a draw is its program and textures, the per-unit sampler requests it consumes,
//   the draw state after the handles, the vertex bytes the layout makes the GPU read
//   (unread gaps in a stride are skipped) and the indices;
// - a clear is its arguments.
// Plain types only: compiled in the SDK domain and by host tests.
#include "gpu_api.h"
#include "vertex_contract.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace twv::gpu {

// Byte ranges of one vertex that the GPU reads, merged and sorted.
struct LayoutSpans {
    uint32_t stride=0,count=0;
    std::array<uint32_t,TwvVertexAttributeCount> begin{},end{};
    bool dense()const noexcept {return count==1 && begin[0]==0 && end[0]==stride;}
};
inline LayoutSpans layout_spans(const TwvVitaVertexLayout& layout) noexcept {
    LayoutSpans s;s.stride=layout.stride;
    std::array<uint32_t,TwvVertexAttributeCount> b{},e{};unsigned n=0;
    for(unsigned a=0;a<TwvVertexAttributeCount;++a) {
        if(layout.offsets[a]==TwvVertexAttributeAbsent)continue;
        const uint64_t end=uint64_t(layout.offsets[a])+uint64_t(layout.components[a])*4u;
        if(end>layout.stride)continue; // malformed: the backend rejects it; hash nothing out of range
        b[n]=layout.offsets[a];e[n]=uint32_t(end);++n;
    }
    for(unsigned i=1;i<n;++i)for(unsigned j=i;j>0 && b[j]<b[j-1];--j) {
        std::swap(b[j],b[j-1]);std::swap(e[j],e[j-1]);
    }
    for(unsigned i=0;i<n;++i) {
        if(s.count && b[i]<=s.end[s.count-1]) {
            if(e[i]>s.end[s.count-1])s.end[s.count-1]=e[i];
        } else {s.begin[s.count]=b[i];s.end[s.count]=e[i];++s.count;}
    }
    return s;
}

struct DigestFrame {
    uint64_t digest=0;
    uint32_t draws=0,clears=0,vertices=0,indices=0,textureWrites=0,programs=0;
};

class DrawDigest {
public:
    DrawDigest() {XXH3_64bits_reset(&frame_);}

    void program_created(uint32_t id,const char* vertex,const char* fragment) {
        if(!id)return;
        const uint64_t v=hash_text(vertex),f=hash_text(fragment);
        const uint64_t pair[2]={v,f};
        programs_[id]=XXH3_64bits(pair,sizeof pair);
        ++current_.programs;
    }
    void texture_created(uint32_t id,uint32_t width,uint32_t height,uint32_t levels) {
        if(!id)return;
        const uint32_t header[4]={0x5a45524fu,width,height,levels}; // "ZERO": zero-filled storage
        textures_[id]=Texture{XXH3_64bits(header,sizeof header)};
    }
    void texture_static(uint32_t id,uint32_t width,uint32_t height,uint32_t levels,const void* bytes,size_t size) {
        if(!id)return;
        XXH3_state_t s;XXH3_64bits_reset(&s);
        const uint32_t header[4]={0x53544154u,width,height,levels}; // "STAT"
        XXH3_64bits_update(&s,header,sizeof header);
        if(bytes && size)XXH3_64bits_update(&s,bytes,size);
        textures_[id]=Texture{XXH3_64bits_digest(&s)};
    }
    void texture_written(uint32_t id,uint32_t level,uint32_t width,uint32_t height,const void* rgba) {
        auto found=textures_.find(id);
        if(found==textures_.end())found=textures_.emplace(id,Texture{0}).first;
        XXH3_state_t s;XXH3_64bits_reset(&s);
        const uint64_t header[3]={found->second.content,(uint64_t(level)<<32)|width,height};
        XXH3_64bits_update(&s,header,sizeof header);
        if(rgba)XXH3_64bits_update(&s,rgba,size_t(width)*height*4u);
        found->second.content=XXH3_64bits_digest(&s);
        ++current_.textureWrites;
    }
    void texture_deleted(uint32_t id) {textures_.erase(id);}
    void texture_sampler(uint32_t id,int min,int mag,int s,int t) {
        auto found=textures_.find(id);
        if(found==textures_.end())return;
        found->second.sampler={1,min,mag,s,t};
    }
    // Per-unit requests are consumed by the next draw (gpu_api.h contract).
    void sampler_unit(uint32_t unit,int min,int mag,int s,int t,int accepted) {
        if(unit>=8)return;
        if(!accepted){pendingFailed_=true;return;}
        pending_[unit]={1,min,mag,s,t};pendingMask_|=1u<<unit;
    }
    void clear(float r,float g,float b,float a,float depth,unsigned mask,int x,int y,int w,int h) {
        XXH3_state_t s;XXH3_64bits_reset(&s);
        const float colour[5]={r,g,b,a,depth};
        const int32_t rect[6]={0x434c5200,int32_t(mask),x,y,w,h}; // "CLR"
        XXH3_64bits_update(&s,rect,sizeof rect);
        XXH3_64bits_update(&s,colour,sizeof colour);
        add(XXH3_64bits_digest(&s));++current_.clears;
    }
    // Returns this draw's own hash (also folded into the frame). G2 draws are hashed in their
    // canonical G1 form: packed attributes unpacked to F32 in the G1 layout, native strips,
    // fans and quads expanded to the triangle indices Aurora generates, G2 fields left out.
    uint64_t draw(const TwvVitaDrawState& state,const void* vertices,uint32_t vertexCount,
                  const uint16_t* indices,uint32_t indexCount) {
        if(state.primitive!=TwvPrimitiveTriangles) {
            expand_primitive(state.primitive,vertexCount);
            indices=canonicalIndices_.data();indexCount=uint32_t(canonicalIndices_.size());
        }
        bool packed=false;
        for(unsigned a=0;a<TwvVertexAttributeCount;++a)packed|=state.vertex_formats[a]!=TwvFormatF32;
        if(packed || state.primitive!=TwvPrimitiveTriangles) {
            canonicalState_=state;
            if(packed && vertices) {
                vertices=unpack_vertices(state,static_cast<const unsigned char*>(vertices),vertexCount);
                canonicalState_.vertex_layout=twv::vgl::float_layout_of(state.vertex_layout);
            }
            return draw_canonical(canonicalState_,vertices,vertexCount,indices,indexCount);
        }
        return draw_canonical(state,vertices,vertexCount,indices,indexCount);
    }
private:
    uint64_t draw_canonical(const TwvVitaDrawState& state,const void* vertices,uint32_t vertexCount,
                            const uint16_t* indices,uint32_t indexCount) {
        XXH3_state_t s;XXH3_64bits_reset(&s);
        const uint32_t counts[4]={0x44524157u,vertexCount,indexCount,pendingMask_|(pendingFailed_?0x100u:0u)}; // "DRAW"
        XXH3_64bits_update(&s,counts,sizeof counts);
        const uint64_t program=find_program(state.program);
        XXH3_64bits_update(&s,&program,sizeof program);
        for(unsigned i=0;i<8;++i) {
            const Texture texture=find_texture(state.textures[i]);
            const auto& o=texture.sampler;
            const uint32_t words[7]={uint32_t(texture.content),uint32_t(texture.content>>32),
                uint32_t(o.set),uint32_t(o.min),uint32_t(o.mag),uint32_t(o.s),uint32_t(o.t)};
            XXH3_64bits_update(&s,words,sizeof words);
            if(pendingMask_&(1u<<i)) {
                const auto& p=pending_[i];
                const int32_t unit[4]={p.min,p.mag,p.s,p.t};
                XXH3_64bits_update(&s,unit,sizeof unit);
            }
        }
        pendingMask_=0;pendingFailed_=false;
        const auto* tail=reinterpret_cast<const unsigned char*>(&state.depth_test);
        XXH3_64bits_update(&s,tail,offsetof(TwvVitaDrawState,primitive)-offsetof(TwvVitaDrawState,depth_test));
        if(vertices && vertexCount) {
            if(!layoutValid_ || std::memcmp(&layout_,&state.vertex_layout,sizeof layout_)!=0) {
                std::memcpy(&layout_,&state.vertex_layout,sizeof layout_);
                spans_=layout_spans(layout_);layoutValid_=true;
            }
            const auto* bytes=static_cast<const unsigned char*>(vertices);
            if(spans_.dense())XXH3_64bits_update(&s,bytes,size_t(vertexCount)*spans_.stride);
            else for(uint32_t v=0;v<vertexCount;++v)for(uint32_t k=0;k<spans_.count;++k)
                XXH3_64bits_update(&s,bytes+size_t(v)*spans_.stride+spans_.begin[k],spans_.end[k]-spans_.begin[k]);
        }
        if(indices && indexCount)XXH3_64bits_update(&s,indices,size_t(indexCount)*sizeof *indices);
        const uint64_t value=XXH3_64bits_digest(&s);
        add(value);
        ++current_.draws;current_.vertices+=vertexCount;current_.indices+=indexCount;
        return value;
    }
public:
    DigestFrame end_frame() {
        current_.digest=XXH3_64bits_digest(&frame_);
        const DigestFrame result=current_;
        current_={};XXH3_64bits_reset(&frame_);
        return result;
    }
    size_t live_textures()const noexcept {return textures_.size();}
    size_t live_programs()const noexcept {return programs_.size();}
private:
    struct Sampler {int32_t set=0,min=0,mag=0,s=0,t=0;};
    struct Texture {uint64_t content=0;Sampler sampler{};};
    // Program sources are hashed without a G2 /*G2{*/.../*}G2*/ section (packed-format scales).
    static uint64_t hash_text(const char* text) {
        if(!text)return 0;
        const char* begin=std::strstr(text,"/*G2{*/");
        const char* end=begin?std::strstr(begin,"/*}G2*/"):nullptr;
        if(!begin || !end)return XXH3_64bits(text,std::strlen(text));
        XXH3_state_t s;XXH3_64bits_reset(&s);
        XXH3_64bits_update(&s,text,size_t(begin-text));
        const char* rest=end+7;
        XXH3_64bits_update(&s,rest,std::strlen(rest));
        return XXH3_64bits_digest(&s);
    }
    uint64_t find_program(uint32_t id)const {
        const auto found=programs_.find(id);return found==programs_.end()?0:found->second;
    }
    Texture find_texture(uint32_t id)const {
        if(!id)return {};
        const auto found=textures_.find(id);
        return found==textures_.end()?Texture{1}:found->second; // 1: unknown handle
    }
    void add(uint64_t value) {XXH3_64bits_update(&frame_,&value,sizeof value);}
    // aurora lib/gx/command_processor.cpp prepare_idx_buffer, from vertex 0.
    void expand_primitive(uint32_t primitive,uint32_t vertexCount) {
        canonicalIndices_.clear();
        const auto push=[&](uint32_t a,uint32_t b,uint32_t c) {
            canonicalIndices_.push_back(uint16_t(a));canonicalIndices_.push_back(uint16_t(b));
            canonicalIndices_.push_back(uint16_t(c));
        };
        if(primitive==TwvPrimitiveQuads) {
            for(uint32_t v=0;v+3<vertexCount;v+=4){push(v,v+1,v+2);push(v+2,v+3,v);}
        } else {
            for(uint32_t v=0;v<vertexCount;++v) {
                if(v<3){canonicalIndices_.push_back(uint16_t(v));continue;}
                if(primitive==TwvPrimitiveFan)push(0,v-1,v);
                else if((v&1)==0)push(v-2,v-1,v);
                else push(v-1,v-2,v);
            }
        }
    }
    const unsigned char* unpack_vertices(const TwvVitaDrawState& state,const unsigned char* src,uint32_t vertexCount) {
        const auto& packed=state.vertex_layout;
        const auto floats=twv::vgl::float_layout_of(packed);
        canonicalVertices_.assign(size_t(vertexCount)*floats.stride,0);
        for(uint32_t v=0;v<vertexCount;++v) {
            const unsigned char* in=src+size_t(v)*packed.stride;
            float* out=reinterpret_cast<float*>(canonicalVertices_.data()+size_t(v)*floats.stride);
            for(unsigned a=0;a<TwvVertexAttributeCount;++a) {
                if(packed.offsets[a]==TwvVertexAttributeAbsent)continue;
                const uint8_t format=state.vertex_formats[a];
                const unsigned count=format==TwvFormatF32?packed.components[a]:state.vertex_counts[a];
                float values[4];
                twv::vgl::unpack_attribute(in+packed.offsets[a],format,count,state.vertex_fracs[a],a,
                                           packed.components[a],values);
                std::memcpy(out+floats.offsets[a]/4,values,packed.components[a]*4u);
            }
        }
        return canonicalVertices_.data();
    }
    XXH3_state_t frame_{};
    DigestFrame current_{};
    std::unordered_map<uint32_t,uint64_t> programs_;
    std::unordered_map<uint32_t,Texture> textures_;
    std::array<Sampler,8> pending_{};
    uint32_t pendingMask_=0;bool pendingFailed_=false;
    TwvVitaVertexLayout layout_{};LayoutSpans spans_{};bool layoutValid_=false;
    TwvVitaDrawState canonicalState_{};
    std::vector<uint16_t> canonicalIndices_;
    std::vector<unsigned char> canonicalVertices_;
};

} // namespace twv::gpu
