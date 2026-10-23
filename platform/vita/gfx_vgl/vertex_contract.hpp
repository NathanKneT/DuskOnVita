#pragma once
#include "gpu_api.h"
#include "color_lut.hpp"
#include "fixed_scale.hpp"
#include <array>
#include <cstring>

namespace twv::vgl {
constexpr unsigned VertexUniformVectors = 4+30+30+8*5+4+4+8;
static_assert(VertexUniformVectors==120 && VertexUniformVectors<=128);
constexpr unsigned vertex_uniform_vectors(bool lighting,unsigned texgenRows,unsigned lights=8) {
    return (lighting ? (lights ? 4+30+30+5*lights+4+4+8 : 4+30+4+4) : 4+30+4)+texgenRows;
}
constexpr bool texgen_uniforms_fit(bool lighting,unsigned rows) {
    return vertex_uniform_vectors(lighting,rows)<=128;
}

// GL attribute locations are shared by the Cg generator and SDK bridge.
constexpr uint32_t AttributeComponents[TwvVertexAttributeCount]={3,4,4,3,3,3,3,3,3,3,3,3,1};
inline TwvVitaVertexLayout compact_vertex_layout(uint32_t texMask,uint32_t colorMask,bool normal,bool rawST=false) {
    TwvVitaVertexLayout result{};
    for(auto& offset:result.offsets) offset=TwvVertexAttributeAbsent;
    for(unsigned a=0;a<TwvVertexAttributeCount;++a)
        result.components[a]=(rawST && a>=3 && a<=10)?2:AttributeComponents[a];
    const auto add=[&](unsigned attr) {
        result.offsets[attr]=result.stride;
        result.stride+=result.components[attr]*sizeof(float);
    };
    add(0); add(12);
    if(normal) add(11);
    for(unsigned ch=0;ch<2;++ch) if(colorMask&(1u<<ch)) add(1+ch);
    for(unsigned tc=0;tc<8;++tc) if(texMask&(1u<<tc)) add(3+tc);
    return result;
}
inline TwvVitaVertexLayout legacy_vertex_layout() {
    auto result=compact_vertex_layout(0,0,false);
    result.stride=sizeof(TwvVitaVertex);
    result.offsets[12]=TwvVertexAttributeAbsent;
    for(unsigned ch=0;ch<2;++ch)result.offsets[1+ch]=offsetof(TwvVitaVertex,color)+ch*4*sizeof(float);
    for(unsigned tc=0;tc<8;++tc)result.offsets[3+tc]=offsetof(TwvVitaVertex,texcoord)+tc*3*sizeof(float);
    return result;
}
inline void store_vertex_attribute(void* vertex,const TwvVitaVertexLayout& layout,unsigned attr,const void* values) {
    if(layout.offsets[attr]!=TwvVertexAttributeAbsent)
        std::memcpy(static_cast<unsigned char*>(vertex)+layout.offsets[attr],values,
                    layout.components[attr]*sizeof(float));
}

// G2 packed formats (GXM backend): GX integer attributes keep their size instead of becoming
// F32. layout.components stay the float counts the shader sees (the G1 meaning); format/count/
// frac give each attribute's stored format, stored component count and fixed-point shift.
// All zero = every attribute F32, the G1 layout.
struct PackedFormats {
    std::array<uint8_t,TwvVertexAttributeCount> format{},count{};
    std::array<int8_t,TwvVertexAttributeCount> frac{};
    bool any()const noexcept {for(auto f:format)if(f)return true;return false;}
};
constexpr unsigned packed_format_bytes(uint8_t f) {
    return f==TwvFormatF32?4u:(f==TwvFormatS16 || f==TwvFormatU16)?2u:1u;
}
// The order compact_vertex_layout assigns offsets in.
constexpr unsigned LayoutOrder[TwvVertexAttributeCount]={0,12,11,1,2,3,4,5,6,7,8,9,10};
// The F32 layout the G1 path builds for the same attributes and component counts.
inline TwvVitaVertexLayout float_layout_of(const TwvVitaVertexLayout& packed) {
    TwvVitaVertexLayout result{};
    for(auto& offset:result.offsets)offset=TwvVertexAttributeAbsent;
    for(unsigned a=0;a<TwvVertexAttributeCount;++a)result.components[a]=packed.components[a];
    for(unsigned attr:LayoutOrder)if(packed.offsets[attr]!=TwvVertexAttributeAbsent) {
        result.offsets[attr]=result.stride;result.stride+=result.components[attr]*4u;
    }
    return result;
}
// Packed layout for the same attributes: same order, each attribute 4-byte aligned (GXM).
inline TwvVitaVertexLayout packed_layout_of(const TwvVitaVertexLayout& floats,const PackedFormats& f) {
    TwvVitaVertexLayout result=floats;result.stride=0;
    for(unsigned attr:LayoutOrder)if(floats.offsets[attr]!=TwvVertexAttributeAbsent) {
        const unsigned count=f.format[attr]?f.count[attr]:floats.components[attr];
        result.offsets[attr]=result.stride;
        result.stride+=(count*packed_format_bytes(f.format[attr])+3u)&~3u;
    }
    return result;
}
// A packed layout with its packed slots marked absent: what the F32 stream packer writes.
inline TwvVitaVertexLayout packed_slots_absent(const TwvVitaVertexLayout& packed,const PackedFormats& f) {
    TwvVitaVertexLayout result=packed;
    for(unsigned a=0;a<TwvVertexAttributeCount;++a)if(f.format[a]!=TwvFormatF32)result.offsets[a]=TwvVertexAttributeAbsent;
    return result;
}
// One attribute as the F32 path writes it: stored components scaled exactly (GX shifts are
// powers of two, every 8/16-bit value is exact in binary32), U8N through the decoder's colour
// table, missing components 0 except a texture coordinate's third (1, as the F32 packer).
inline void unpack_attribute(const uint8_t* src,uint8_t format,unsigned count,int frac,unsigned attr,
                             unsigned components,float* out) {
    const float scale=fixed_point_scale(uint32_t(frac<0?0:frac));
    for(unsigned k=0;k<components;++k) {
        float v=(k==2 && attr>=3 && attr<=10)?1.f:0.f;
        if(k<count) {
            const uint8_t* p=src+k*packed_format_bytes(format);
            if(format==TwvFormatF32)std::memcpy(&v,p,4);
            else if(format==TwvFormatS16){int16_t r;std::memcpy(&r,p,2);v=float(r)*scale;}
            else if(format==TwvFormatU16){uint16_t r;std::memcpy(&r,p,2);v=float(r)*scale;}
            else if(format==TwvFormatS8)v=float(int8_t(*p))*scale;
            else if(format==TwvFormatU8)v=float(*p)*scale;
            else v=ColorUnorm<8>[*p];
        }
        out[k]=v;
    }
}
// Exact inverse for the reference decoders, which produce the F32 values: false when a value
// is not exactly representable (a validation failure, never a silent rounding).
inline bool pack_attribute(uint8_t* dst,uint8_t format,unsigned count,int frac,const float* values) {
    const float up=fixed_point_scale(0)/fixed_point_scale(uint32_t(frac<0?0:frac)); // 2^frac, exact
    for(unsigned k=0;k<count;++k) {
        uint8_t* p=dst+k*packed_format_bytes(format);
        const float v=values[k];
        if(format==TwvFormatF32){std::memcpy(p,&v,4);continue;}
        if(format==TwvFormatU8N) {
            const float scaled=v*255.f;
            if(!(scaled>=0.f && scaled<=255.f))return false;
            const auto raw=uint8_t(scaled+0.5f);
            if(ColorUnorm<8>[raw]!=v)return false;
            *p=raw;continue;
        }
        const float scaled=v*up;
        const float lo=format==TwvFormatS16?-32768.f:format==TwvFormatS8?-128.f:0.f;
        const float hi=format==TwvFormatS16?32767.f:format==TwvFormatU16?65535.f:format==TwvFormatS8?127.f:255.f;
        if(!(scaled>=lo && scaled<=hi) || float(int32_t(scaled))!=scaled)return false;
        const int32_t raw=int32_t(scaled);
        if(format==TwvFormatS16 || format==TwvFormatU16){const uint16_t r=uint16_t(raw);std::memcpy(p,&r,2);}
        else *p=uint8_t(raw);
    }
    return true;
}

template<class Matrix>
void pack_matrix_rows(float* dst,const Matrix& matrix) {
    for(unsigned k=0;k<4;++k) {
        dst[k]=matrix.m0[k]; dst[4+k]=matrix.m1[k]; dst[8+k]=matrix.m2[k];
    }
}

// Only raw packet indices select this extent. Matrix values remain live and no
// slot is remapped. Shader declarations stay at ten slots: no extra programs or
// first-use compilation. The caller has rejected every index outside [0,9].
template<class Palette>
void pack_matrix_palette(float* pos,float* nrm,const Palette& matrices,unsigned slots,bool lighting) {
    for(unsigned i=0;i<slots;++i) {
        pack_matrix_rows(pos+i*12,matrices[i].pos);
        if(lighting)pack_matrix_rows(nrm+i*12,matrices[i].nrm);
    }
}

inline void pack_light_mask(float* dst,uint32_t mask) {
    for(unsigned i=0;i<8;++i) dst[i]=(mask&(1u<<i))?1.f:0.f;
}
}
