#pragma once
// Attribute-major packing: choose format/indexing/endian once outside a vertex
// loop. Pure byte reads; no captured source values, GPU state or heap storage.
#include "color_lut.hpp"
#include "fixed_scale.hpp"
#include <dolphin/gx/GXEnum.h>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#ifndef TWV_VITA_NEON_DECODE
#define TWV_VITA_NEON_DECODE 0
#endif
#if TWV_VITA_NEON_DECODE && defined(__ARM_NEON)
#include <arm_neon.h>
#define TWV_STREAM_NEON 1
#else
#define TWV_STREAM_NEON 0
#endif

namespace twv::vgl {
// NEON kernels for the integer numeric formats (positions, normals, texture coordinates), chosen
// at start-up. GX scales are exact powers of two (fixed_scale.hpp) and every 8/16-bit value is
// exact in binary32, so the converted and scaled values are bit-identical to the scalar loops
// (no product is subnormal for frac < 32). Verify recomputes every NEON vertex with the scalar
// loop, counts any difference and keeps the scalar bytes. The counters cover every packed vertex.
enum class StreamNeonMode : int { Off = 0, On = 1, Verify = 2 };
struct StreamDecodeStats {
    StreamNeonMode mode = StreamNeonMode::Off;
    bool selected = false;
    uint64_t neonVertices = 0, verifiedVertices = 0, mismatches = 0;
    // Vertices packed per kernel: [0 numeric 3F, 1 numeric ST, 2 colour][compType 0..5][direct, i8, i16].
    std::array<uint64_t, 3 * 6 * 3> vertices{};
};
inline StreamDecodeStats& stream_decode_stats() noexcept { static StreamDecodeStats s; return s; }
inline void count_stream_vertices(unsigned kind, unsigned compType, int attrType, uint32_t vertices) noexcept {
    const unsigned index = attrType == GX_DIRECT ? 0 : attrType == GX_INDEX8 ? 1 : 2;
    stream_decode_stats().vertices[(kind * 6 + (compType < 6 ? compType : 5)) * 3 + index] += vertices;
}
template<bool Little> inline uint16_t stream_u16(const uint8_t* p) noexcept {
    uint16_t v;std::memcpy(&v,p,2);
    if constexpr(Little!=(std::endian::native==std::endian::little))v=__builtin_bswap16(v);
    return v;
}
template<bool Little> inline uint32_t stream_u32(const uint8_t* p) noexcept {
    uint32_t v;std::memcpy(&v,p,4);
    if constexpr(Little!=(std::endian::native==std::endian::little))v=__builtin_bswap32(v);
    return v;
}
template<class Attr> struct AttributeStream {
    const Attr& attr;
    const uint8_t* packet;
    const uint8_t* array;
    uint32_t packetStride;
    template<int Index> const uint8_t* at(uint32_t vertex) const noexcept {
        const auto* p=packet+size_t(vertex)*packetStride+attr.offset;
        if constexpr(Index==GX_DIRECT)return p;
        else {
            const uint32_t index=Index==GX_INDEX8?*p:stream_u16<false>(p);
            return array+size_t(index)*attr.stride;
        }
    }
};

template<int Type,int Index,bool Little,class Attr>
void numeric_attribute_stream_scalar(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t outputStride,uint32_t vertices,unsigned components,float third) noexcept {
    float scale=1.f;
    if constexpr(Type!=GX_F32)scale=fixed_point_scale(in.attr.frac);
    for(uint32_t v=0;v<vertices;++v,out+=outputStride) {
        const auto* source=in.template at<Index>(v);
        float value[3]={0.f,0.f,third};
        for(unsigned i=0;i<components;++i) {
            if constexpr(Type==GX_U8)value[i]=float(source[i])*scale;
            else if constexpr(Type==GX_S8)value[i]=float(int8_t(source[i]))*scale;
            else if constexpr(Type==GX_U16)value[i]=float(stream_u16<Little>(source+2*i))*scale;
            else if constexpr(Type==GX_S16)value[i]=float(int16_t(stream_u16<Little>(source+2*i)))*scale;
            else {const auto bits=stream_u32<Little>(source+4*i);std::memcpy(value+i,&bits,4);}
        }
        std::memcpy(out,value,12);
    }
}
// Return F32 payloads as bits: no floating-point operation may quiet a NaN or
// change signed zero. Integer formats use the same exact scale as the 3F API.
template<int Type,bool Little>
inline uint32_t numeric_st_component(const uint8_t* source,float scale) noexcept {
    if constexpr(Type==GX_F32)return stream_u32<Little>(source);
    else {
        float value;
        if constexpr(Type==GX_U8)value=float(*source)*scale;
        else if constexpr(Type==GX_S8)value=float(int8_t(*source))*scale;
        else if constexpr(Type==GX_U16)value=float(stream_u16<Little>(source))*scale;
        else value=float(int16_t(stream_u16<Little>(source)))*scale;
        uint32_t bits;std::memcpy(&bits,&value,4);return bits;
    }
}

template<int Type,int Index,bool Little,unsigned Components,class Attr>
void numeric_attribute_st_stream_scalar(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t outputStride,uint32_t vertices) noexcept {
    constexpr unsigned width=Type==GX_U8 || Type==GX_S8 ? 1 : Type==GX_F32 ? 4 : 2;
    float scale=1.f;
    if constexpr(Type!=GX_F32)scale=fixed_point_scale(in.attr.frac);
    for(uint32_t v=0;v<vertices;++v,out+=outputStride) {
        const auto* source=in.template at<Index>(v);
        const uint32_t s=numeric_st_component<Type,Little>(source,scale);
        uint32_t t=0; // A one-component coordinate expands with positive-zero T.
        if constexpr(Components==2)t=numeric_st_component<Type,Little>(source+width,scale);
        std::memcpy(out,&s,4);std::memcpy(out+4,&t,4);
    }
}

template<int Type,bool Little>
inline int32_t stream_int(const uint8_t* source,unsigned component) noexcept {
    if constexpr(Type==GX_U8)return source[component];
    else if constexpr(Type==GX_S8)return int8_t(source[component]);
    else if constexpr(Type==GX_U16)return stream_u16<Little>(source+2*component);
    else return int16_t(stream_u16<Little>(source+2*component));
}
#if TWV_STREAM_NEON
// Four vertices per step: their integers are gathered as before, then converted and scaled in
// NEON lanes; the stores write the scalar loop's bytes. Returns how many vertices it packed.
template<int Type,int Index,bool Little,unsigned Components,class Attr>
uint32_t numeric_neon(const AttributeStream<Attr>& in,uint8_t* out,uint32_t outputStride,
        uint32_t vertices,float scale) noexcept {
    const float32x4_t s=vdupq_n_f32(scale);
    uint32_t v=0;
    for(;v+4<=vertices;v+=4) {
        alignas(16) int32_t raw[4*Components];
        for(unsigned k=0;k<4;++k) {
            const auto* p=in.template at<Index>(v+k);
            for(unsigned i=0;i<Components;++i)raw[k*Components+i]=stream_int<Type,Little>(p,i);
        }
        alignas(16) float value[4*Components];
        for(unsigned q=0;q<Components;++q)
            vst1q_f32(value+4*q,vmulq_f32(vcvtq_f32_s32(vld1q_s32(raw+4*q)),s));
        for(unsigned k=0;k<4;++k)std::memcpy(out+size_t(v+k)*outputStride,value+k*Components,4*Components);
    }
    return v;
}
#endif
template<int Type,int Index,bool Little,class Attr>
void numeric_attribute_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t outputStride,uint32_t vertices,unsigned components,float third) noexcept {
#if TWV_STREAM_NEON
    if constexpr(Type!=GX_F32) {
        auto& stats=stream_decode_stats();
        if(stats.mode!=StreamNeonMode::Off && components==3 && in.attr.frac<32) {
            const uint32_t done=numeric_neon<Type,Index,Little,3>(in,out,outputStride,vertices,
                fixed_point_scale(in.attr.frac));
            stats.neonVertices+=done;
            if(stats.mode==StreamNeonMode::Verify) {
                for(uint32_t v=0;v<done;++v) {
                    uint8_t expected[12];
                    const AttributeStream<Attr> one{in.attr,in.packet+size_t(v)*in.packetStride,in.array,in.packetStride};
                    numeric_attribute_stream_scalar<Type,Index,Little>(one,expected,12,1,components,third);
                    ++stats.verifiedVertices;
                    if(std::memcmp(expected,out+size_t(v)*outputStride,12)) {
                        ++stats.mismatches;std::memcpy(out+size_t(v)*outputStride,expected,12);
                    }
                }
            }
            const AttributeStream<Attr> rest{in.attr,in.packet+size_t(done)*in.packetStride,in.array,in.packetStride};
            numeric_attribute_stream_scalar<Type,Index,Little>(rest,out+size_t(done)*outputStride,outputStride,
                vertices-done,components,third);
            return;
        }
    }
#endif
    numeric_attribute_stream_scalar<Type,Index,Little>(in,out,outputStride,vertices,components,third);
}
template<int Type,int Index,bool Little,unsigned Components,class Attr>
void numeric_attribute_st_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t outputStride,uint32_t vertices) noexcept {
#if TWV_STREAM_NEON
    if constexpr(Type!=GX_F32 && Components==2) {
        auto& stats=stream_decode_stats();
        if(stats.mode!=StreamNeonMode::Off && in.attr.frac<32) {
            const uint32_t done=numeric_neon<Type,Index,Little,2>(in,out,outputStride,vertices,
                fixed_point_scale(in.attr.frac));
            stats.neonVertices+=done;
            if(stats.mode==StreamNeonMode::Verify) {
                for(uint32_t v=0;v<done;++v) {
                    uint8_t expected[8];
                    const AttributeStream<Attr> one{in.attr,in.packet+size_t(v)*in.packetStride,in.array,in.packetStride};
                    numeric_attribute_st_stream_scalar<Type,Index,Little,Components>(one,expected,8,1);
                    ++stats.verifiedVertices;
                    if(std::memcmp(expected,out+size_t(v)*outputStride,8)) {
                        ++stats.mismatches;std::memcpy(out+size_t(v)*outputStride,expected,8);
                    }
                }
            }
            const AttributeStream<Attr> rest{in.attr,in.packet+size_t(done)*in.packetStride,in.array,in.packetStride};
            numeric_attribute_st_stream_scalar<Type,Index,Little,Components>(rest,out+size_t(done)*outputStride,
                outputStride,vertices-done);
            return;
        }
    }
#endif
    numeric_attribute_st_stream_scalar<Type,Index,Little,Components>(in,out,outputStride,vertices);
}

template<int Type,bool Little,unsigned Components,class Attr>
void indexed_attribute_st_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices) noexcept {
    if(in.attr.attrType==GX_DIRECT)
        numeric_attribute_st_stream<Type,GX_DIRECT,false,Components>(in,out,stride,vertices);
    else if(in.attr.attrType==GX_INDEX8)
        numeric_attribute_st_stream<Type,GX_INDEX8,Little,Components>(in,out,stride,vertices);
    else numeric_attribute_st_stream<Type,GX_INDEX16,Little,Components>(in,out,stride,vertices);
}

template<int Type,unsigned Components,class Attr>
void ordered_attribute_st_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices) noexcept {
    if(in.attr.le)indexed_attribute_st_stream<Type,true,Components>(in,out,stride,vertices);
    else indexed_attribute_st_stream<Type,false,Components>(in,out,stride,vertices);
}

template<unsigned Components,class Attr>
void typed_attribute_st_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices) noexcept {
    switch(in.attr.compType) {
    case GX_U8:ordered_attribute_st_stream<GX_U8,Components>(in,out,stride,vertices);break;
    case GX_S8:ordered_attribute_st_stream<GX_S8,Components>(in,out,stride,vertices);break;
    case GX_U16:ordered_attribute_st_stream<GX_U16,Components>(in,out,stride,vertices);break;
    case GX_S16:ordered_attribute_st_stream<GX_S16,Components>(in,out,stride,vertices);break;
    default:ordered_attribute_st_stream<GX_F32,Components>(in,out,stride,vertices);break;
    }
}

// ST output only: format/index/endian/component dispatch occurs once per packet.
// Reads the first min(components,2) values (components must be 1..3), writes exactly
// 8 bytes per vertex, and leaves stride padding untouched. The existing 3F API
// stays unchanged. Zero vertices permits null input and output pointers.
template<class Attr>
void pack_numeric_attribute_st(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices,unsigned components) noexcept {
    count_stream_vertices(1,unsigned(in.attr.compType),in.attr.attrType,vertices);
    if(components==1)typed_attribute_st_stream<1>(in,out,stride,vertices);
    else typed_attribute_st_stream<2>(in,out,stride,vertices);
}

template<int Type,int Index,bool Little,class Attr>
void color_attribute_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t outputStride,uint32_t vertices) noexcept {
    for(uint32_t v=0;v<vertices;++v,out+=outputStride) {
        const auto* p=in.template at<Index>(v);float value[4];
        if constexpr(Type==GX_RGB565) {
            const uint16_t bits=stream_u16<Little>(p);
            value[0]=normalized_color<5>((bits>>11)&31);value[1]=normalized_color<6>((bits>>5)&63);
            value[2]=normalized_color<5>(bits&31);value[3]=1.f;
        } else if constexpr(Type==GX_RGBA4) {
            const uint16_t bits=stream_u16<Little>(p);
            for(unsigned i=0;i<4;++i)value[i]=normalized_color<4>((bits>>(12-4*i))&15);
        } else if constexpr(Type==GX_RGBA6) {
            const uint32_t bits=Little?(uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)):
                ((uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2]);
            for(unsigned i=0;i<4;++i)value[i]=normalized_color<6>((bits>>(18-6*i))&63);
        } else {
            for(unsigned i=0;i<3;++i)value[i]=normalized_color<8>(p[i]);
            if constexpr(Type==GX_RGB8 || Type==GX_RGBX8)value[3]=1.f;
            else value[3]=normalized_color<8>(p[3]);
        }
        std::memcpy(out,value,16);
    }
}
template<int Type,bool Little,bool Color,class Attr>
void indexed_attribute_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices,unsigned components,float third) noexcept {
    if(in.attr.attrType==GX_DIRECT) {
        // Direct GX bytes always use wire big endian, even if attr.le is set.
        if constexpr(Color)color_attribute_stream<Type,GX_DIRECT,false>(in,out,stride,vertices);
        else numeric_attribute_stream<Type,GX_DIRECT,false>(in,out,stride,vertices,components,third);
    } else if(in.attr.attrType==GX_INDEX8) {
        if constexpr(Color)color_attribute_stream<Type,GX_INDEX8,Little>(in,out,stride,vertices);
        else numeric_attribute_stream<Type,GX_INDEX8,Little>(in,out,stride,vertices,components,third);
    } else {
        if constexpr(Color)color_attribute_stream<Type,GX_INDEX16,Little>(in,out,stride,vertices);
        else numeric_attribute_stream<Type,GX_INDEX16,Little>(in,out,stride,vertices,components,third);
    }
}
template<int Type,bool Color,class Attr>
void ordered_attribute_stream(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices,unsigned components,float third) noexcept {
    if(in.attr.le)indexed_attribute_stream<Type,true,Color>(in,out,stride,vertices,components,third);
    else indexed_attribute_stream<Type,false,Color>(in,out,stride,vertices,components,third);
}
template<class Attr>
void pack_numeric_attribute(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices,unsigned components,float third=0.f) noexcept {
    count_stream_vertices(0,unsigned(in.attr.compType),in.attr.attrType,vertices);
    switch(in.attr.compType) {
    case GX_U8:ordered_attribute_stream<GX_U8,false>(in,out,stride,vertices,components,third);break;
    case GX_S8:ordered_attribute_stream<GX_S8,false>(in,out,stride,vertices,components,third);break;
    case GX_U16:ordered_attribute_stream<GX_U16,false>(in,out,stride,vertices,components,third);break;
    case GX_S16:ordered_attribute_stream<GX_S16,false>(in,out,stride,vertices,components,third);break;
    default:ordered_attribute_stream<GX_F32,false>(in,out,stride,vertices,components,third);break;
    }
}
template<class Attr>
void pack_color_attribute(const AttributeStream<Attr>& in,uint8_t* out,
        uint32_t stride,uint32_t vertices) noexcept {
    count_stream_vertices(2,unsigned(in.attr.compType),in.attr.attrType,vertices);
    switch(in.attr.compType) {
    case GX_RGB565:ordered_attribute_stream<GX_RGB565,true>(in,out,stride,vertices,4,0);break;
    case GX_RGB8:ordered_attribute_stream<GX_RGB8,true>(in,out,stride,vertices,4,0);break;
    case GX_RGBX8:ordered_attribute_stream<GX_RGBX8,true>(in,out,stride,vertices,4,0);break;
    case GX_RGBA4:ordered_attribute_stream<GX_RGBA4,true>(in,out,stride,vertices,4,0);break;
    case GX_RGBA6:ordered_attribute_stream<GX_RGBA6,true>(in,out,stride,vertices,4,0);break;
    default:ordered_attribute_stream<GX_RGBA8,true>(in,out,stride,vertices,4,0);break;
    }
}
// G2 packed formats (GXM backend): the attribute's own integers byte-swapped to little endian,
// or its F32 bits, with no conversion. The vertex shader applies the GX shift (an exact power of
// two); RGBA8 colour bytes go to the GPU as U8N.
// Each attribute slot is 4-byte aligned; its padding is written as zeros, so a packed vertex is
// fully determined by the packet (the caches compare outputs byte for byte).
template<int Type,int Index,bool Little,class Attr>
void packed_attribute_stream(const AttributeStream<Attr>& in,uint8_t* out,uint32_t outputStride,
        uint32_t vertices,unsigned components) noexcept {
    constexpr unsigned size=Type==GX_U8 || Type==GX_S8 ? 1 : Type==GX_F32 ? 4 : 2;
    const unsigned used=components*size,slot=(used+3u)&~3u;
    for(uint32_t v=0;v<vertices;++v,out+=outputStride) {
        const auto* p=in.template at<Index>(v);
        for(unsigned i=0;i<components;++i) {
            if constexpr(Type==GX_U8 || Type==GX_S8)out[i]=p[i];
            else if constexpr(Type==GX_U16 || Type==GX_S16) {
                const uint16_t r=stream_u16<Little>(p+2*i);std::memcpy(out+2*i,&r,2);
            } else {const uint32_t r=stream_u32<Little>(p+4*i);std::memcpy(out+4*i,&r,4);}
        }
        for(unsigned i=used;i<slot;++i)out[i]=0;
    }
}
template<int Type,class Attr>
void packed_attribute_indexed(const AttributeStream<Attr>& in,uint8_t* out,uint32_t stride,uint32_t vertices,
        unsigned components) noexcept {
    if(in.attr.attrType==GX_DIRECT)packed_attribute_stream<Type,GX_DIRECT,false>(in,out,stride,vertices,components);
    else if(in.attr.attrType==GX_INDEX8) {
        if(in.attr.le)packed_attribute_stream<Type,GX_INDEX8,true>(in,out,stride,vertices,components);
        else packed_attribute_stream<Type,GX_INDEX8,false>(in,out,stride,vertices,components);
    } else {
        if(in.attr.le)packed_attribute_stream<Type,GX_INDEX16,true>(in,out,stride,vertices,components);
        else packed_attribute_stream<Type,GX_INDEX16,false>(in,out,stride,vertices,components);
    }
}
template<class Attr>
void pack_packed_numeric(const AttributeStream<Attr>& in,uint8_t* out,uint32_t stride,uint32_t vertices,
        unsigned components) noexcept {
    count_stream_vertices(0,unsigned(in.attr.compType),in.attr.attrType,vertices);
    switch(in.attr.compType) {
    case GX_U8:packed_attribute_indexed<GX_U8>(in,out,stride,vertices,components);break;
    case GX_S8:packed_attribute_indexed<GX_S8>(in,out,stride,vertices,components);break;
    case GX_U16:packed_attribute_indexed<GX_U16>(in,out,stride,vertices,components);break;
    case GX_S16:packed_attribute_indexed<GX_S16>(in,out,stride,vertices,components);break;
    default:packed_attribute_indexed<GX_F32>(in,out,stride,vertices,components);break;
    }
}
// RGBA8 only (the packed colour format): four bytes R, G, B, A, as the GX stores them.
template<class Attr>
void pack_packed_color(const AttributeStream<Attr>& in,uint8_t* out,uint32_t stride,uint32_t vertices) noexcept {
    count_stream_vertices(2,unsigned(in.attr.compType),in.attr.attrType,vertices);
    packed_attribute_indexed<GX_U8>(in,out,stride,vertices,4);
}
inline void default_attribute_stream(uint8_t* out,uint32_t stride,uint32_t vertices,
        const float* value,unsigned bytes) noexcept {
    for(uint32_t v=0;v<vertices;++v,out+=stride) {
        if(bytes==16)std::memcpy(out,value,16);
        else if(bytes==12)std::memcpy(out,value,12);
        else if(bytes==8)std::memcpy(out,value,8);
        else std::memcpy(out,value,4);
    }
}
} // namespace twv::vgl
