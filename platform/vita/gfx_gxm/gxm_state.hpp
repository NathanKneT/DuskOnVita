#pragma once
// G1: pure mappings from the twv_vgl_* draw state to GXM state values (no SDK headers,
// host-tested in tests/gxm_state_test.cpp; gxm_backend.cpp static_asserts the values
// against the SDK enums). Each rule reproduces what the vitaGL backend asked vitaGL
// for, so the two backends draw the same pixels for the same draw stream.
#include <cstdint>

namespace twv::gxm {
// SceGxm enum values (psp2/gxm.h).
constexpr uint8_t BlendFuncNone=0,BlendFuncAdd=1,BlendFuncReverseSubtract=3;
constexpr uint8_t FactorZero=0,FactorOne=1,FactorSrcColor=2,FactorOneMinusSrcColor=3,FactorSrcAlpha=4,
    FactorOneMinusSrcAlpha=5,FactorDstColor=6,FactorOneMinusDstColor=7,FactorDstAlpha=8,FactorOneMinusDstAlpha=9;
constexpr uint8_t MaskA=1,MaskR=2,MaskG=4,MaskB=8,MaskAll=15;
constexpr uint32_t CullNone=0,CullCw=1,CullCcw=2;
constexpr uint32_t FilterPoint=0,FilterLinear=1,MipFilterDisabled=0,MipFilterEnabled=0x200;
constexpr uint32_t AddrRepeat=0,AddrMirror=1,AddrClamp=2;
constexpr uint32_t LodBiasZero=31; // field value for no bias (vitaGL: 0 + GL_MAX_TEXTURE_LOD_BIAS)
constexpr uint32_t depth_func(int gx) {return uint32_t(gx<0||gx>7?7:gx)<<22;} // NEVER..ALWAYS, same order

struct BlendKey {
    uint8_t colorMask=MaskAll,colorFunc=BlendFuncNone,alphaFunc=BlendFuncNone;
    uint8_t colorSrc=FactorSrcAlpha,colorDst=FactorOneMinusSrcAlpha,alphaSrc=FactorOne,alphaDst=FactorZero;
    uint32_t packed()const {
        return uint32_t(colorMask)|uint32_t(colorFunc)<<8|uint32_t(alphaFunc)<<12|uint32_t(colorSrc)<<16|
               uint32_t(colorDst)<<20|uint32_t(alphaSrc)<<24|uint32_t(alphaDst)<<28;
    }
};
// GX blend factor index (gpu_api.cpp factor()): source and destination differ at 2/3.
constexpr uint8_t blend_factor(int n,bool destination) {
    constexpr uint8_t source[8]={FactorZero,FactorOne,FactorDstColor,FactorOneMinusDstColor,FactorSrcAlpha,
                                 FactorOneMinusSrcAlpha,FactorDstAlpha,FactorOneMinusDstAlpha};
    constexpr uint8_t dest[8]={FactorZero,FactorOne,FactorSrcColor,FactorOneMinusSrcColor,FactorSrcAlpha,
                               FactorOneMinusSrcAlpha,FactorDstAlpha,FactorOneMinusDstAlpha};
    return n>=0 && n<8 ? (destination?dest[n]:source[n]) : FactorOne;
}
// vitaGL patches every fragment program with a blend info: blending off is function NONE
// with its default factors; GX subtract is ONE/ONE with reverse subtraction (dst - src).
constexpr BlendKey blend_key(int blend,int subtract,int src,int dst,int colorWrite,int alphaWrite) {
    BlendKey k;
    k.colorMask=uint8_t((colorWrite?MaskR|MaskG|MaskB:0)|(alphaWrite?MaskA:0));
    if(!blend)return k;
    if(subtract) {
        k.colorFunc=k.alphaFunc=BlendFuncReverseSubtract;
        k.colorSrc=k.colorDst=k.alphaSrc=k.alphaDst=FactorOne;
        return k;
    }
    k.colorFunc=k.alphaFunc=BlendFuncAdd;
    k.colorSrc=k.alphaSrc=blend_factor(src,false);
    k.colorDst=k.alphaDst=blend_factor(dst,true);
    return k;
}
// GX cull 1 = front, 2 = back. The display surface is drawn with a negative Y scale and
// GL_CW front faces; vitaGL then culls CW for GL_FRONT and CCW for GL_BACK.
constexpr uint32_t cull_mode(int gx) {return gx==1?CullCw:gx==2?CullCcw:CullNone;}

// Canonical sampler codes (gfx_vgl/sampler_state.hpp) to GXM, as vitaGL converts the GL
// filters: mip count = all levels only for a mipmap min filter, else 1.
struct SamplerWords {uint32_t minFilter,mipFilter,magFilter,u,v;bool mips;};
constexpr SamplerWords sampler_words(int min,int mag,int s,int t) {
    const int m=min>=0&&min<6?min:1;
    constexpr uint32_t minf[6]={FilterPoint,FilterLinear,FilterPoint,FilterLinear,FilterPoint,FilterLinear};
    constexpr uint32_t mipf[6]={MipFilterDisabled,MipFilterDisabled,MipFilterDisabled,MipFilterDisabled,
                                MipFilterEnabled,MipFilterEnabled};
    const auto wrap=[](int n)->uint32_t{return n==1?AddrRepeat:n==2?AddrMirror:AddrClamp;};
    return {minf[m],mipf[m],mag?FilterLinear:FilterPoint,wrap(s),wrap(t),m>=2};
}

// Display-surface viewport from the GL rectangle (origin bottom-left). Exact halves: equal
// to vitaGL's integer halves for even sizes (vitaGL truncates odd sizes by half a pixel).
struct Viewport {float xOffset,xScale,yOffset,yScale,zOffset,zScale;};
constexpr int DisplayWidth=960,DisplayHeight=544;
constexpr Viewport viewport(int x,int y,int w,int h) {
    return {float(x)+float(w)*0.5f,float(w)*0.5f,float(DisplayHeight-y)-float(h)*0.5f,-float(h)*0.5f,0.5f,0.5f};
}
// GL scissor to GXM region clip (inclusive pixel bounds, origin top-left), clamped as vitaGL does.
struct Region {unsigned xMin,yMin,xMax,yMax;bool empty;};
constexpr Region scissor_region(int gx,int gy,int gw,int gh) {
    int x=gx<0?0:gx,w=gw,h=gh,y=DisplayHeight-gy-gh;
    if(y<0)y=0;
    if(x+w>DisplayWidth)w=DisplayWidth-x;
    if(y+h>DisplayHeight)h=DisplayHeight-y;
    if(w<=0||h<=0)return {0,0,0,0,true};
    return {unsigned(x),unsigned(y),unsigned(x+w-1),unsigned(y+h-1),false};
}
// Clear rectangle (GL origin) in clip space for the full-screen viewport, and the clear depth
// in vitaGL's form (NDC d*2-1, mapped back by z offset/scale 0.5).
struct ClearQuad {float x0,x1,yTop,yBottom,z;};
constexpr ClearQuad clear_quad(int x,int y,int w,int h,float depth) {
    const float top=float(DisplayHeight-y-h);
    return {2.f*float(x)/float(DisplayWidth)-1.f,2.f*float(x+w)/float(DisplayWidth)-1.f,
            1.f-2.f*top/float(DisplayHeight),1.f-2.f*(top+float(h))/float(DisplayHeight),depth*2.f-1.f};
}

// Linear RGBA8 texture with mips, laid out as vitaGL lays it out and GXM reads it. Level 0
// rows are ALIGN(w,8) texels; level k starts after sum(max(W_j,8)*H_j) for j<k, with W_j, H_j
// the sizes rounded up to powers of two and halved per level (equal for power-of-two sizes).
constexpr uint32_t next_pow2(uint32_t v) {uint32_t p=1;while(p<v)p<<=1;return p;}
struct MipLevel {uint32_t offset,stride,width,height;};
constexpr MipLevel mip_level(uint32_t width,uint32_t height,uint32_t level) {
    if(level==0)return {0,(width+7u)&~7u,width,height};
    uint32_t offset=0,w=next_pow2(width),h=next_pow2(height);
    for(uint32_t j=0;j<level;++j) {
        offset+=(w<8?8:w)*h*4u;
        w=w>1?w/2:1;h=h>1?h/2:1;
    }
    const uint32_t lw=width>>level?width>>level:1,lh=height>>level?height>>level:1;
    return {offset,w<8?8:w,lw,lh};
}
constexpr uint32_t texture_bytes(uint32_t width,uint32_t height,uint32_t levels) {
    if(!levels)return 0;
    const auto last=mip_level(width,height,levels-1);
    return last.offset+last.stride*last.height*4u;
}
}
