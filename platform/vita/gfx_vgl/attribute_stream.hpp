#pragma once
// Attribute-major packing: choose format/indexing/endian once outside a vertex
// loop. Pure byte reads; no captured source values, GPU state or heap storage.
#include "color_lut.hpp"
#include "fixed_scale.hpp"
#include <dolphin/gx/GXEnum.h>
#include <bit>
#include <cstdint>
#include <cstring>

namespace twv::vgl {
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
void numeric_attribute_stream(const AttributeStream<Attr>& in,uint8_t* out,
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
void numeric_attribute_st_stream(const AttributeStream<Attr>& in,uint8_t* out,
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
    switch(in.attr.compType) {
    case GX_RGB565:ordered_attribute_stream<GX_RGB565,true>(in,out,stride,vertices,4,0);break;
    case GX_RGB8:ordered_attribute_stream<GX_RGB8,true>(in,out,stride,vertices,4,0);break;
    case GX_RGBX8:ordered_attribute_stream<GX_RGBX8,true>(in,out,stride,vertices,4,0);break;
    case GX_RGBA4:ordered_attribute_stream<GX_RGBA4,true>(in,out,stride,vertices,4,0);break;
    case GX_RGBA6:ordered_attribute_stream<GX_RGBA6,true>(in,out,stride,vertices,4,0);break;
    default:ordered_attribute_stream<GX_RGBA8,true>(in,out,stride,vertices,4,0);break;
    }
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
