#pragma once
#include "gpu_api.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace twv::vgl {
inline constexpr uint32_t RasterViewport=1u<<0,RasterScissorTest=1u<<1,RasterScissor=1u<<2,
    RasterDepthTest=1u<<3,RasterDepthFunc=1u<<4,RasterDepthWrite=1u<<5,
    RasterColorMask=1u<<6,RasterCull=1u<<7,RasterBlend=1u<<8,RasterAll=(1u<<9)-1;
struct RasterState {
    std::array<int,4> viewport{},scissor{};
    int depthTest=0,depthFunc=0,depthWrite=0,colorWrite=0,alphaWrite=0,cull=0,
        blend=0,src=0,dst=0,subtract=0;
};
inline RasterState raster_state(const TwvVitaDrawState& s) noexcept {
    RasterState r;
    std::memcpy(r.viewport.data(),s.viewport,sizeof s.viewport);
    std::memcpy(r.scissor.data(),s.scissor,sizeof s.scissor);
    r.depthTest=s.depth_test;r.depthFunc=s.depth_func;r.depthWrite=s.depth_write;
    r.colorWrite=s.color_write;r.alphaWrite=s.alpha_write;r.cull=s.cull;
    r.blend=s.blend;r.src=s.src_factor;r.dst=s.dst_factor;r.subtract=s.subtract;
    return r;
}
class RasterShadow {
public:
    uint32_t changes(const RasterState& r) const noexcept {
        if(!valid)return RasterAll;
        uint32_t flags=0;
        if(r.viewport!=state.viewport)flags|=RasterViewport;
        if(r.scissor!=state.scissor)flags|=RasterScissor;
        if(r.depthTest!=state.depthTest)flags|=RasterDepthTest;
        if(r.depthFunc!=state.depthFunc)flags|=RasterDepthFunc;
        if(r.depthWrite!=state.depthWrite)flags|=RasterDepthWrite;
        if(r.colorWrite!=state.colorWrite || r.alphaWrite!=state.alphaWrite)flags|=RasterColorMask;
        if(r.cull!=state.cull)flags|=RasterCull;
        // Apply the entire enable/function/equation group on any change. In
        // particular, parameters ignored while disabled must install on enable.
        if(r.blend!=state.blend || r.src!=state.src || r.dst!=state.dst || r.subtract!=state.subtract)
            flags|=RasterBlend;
        return flags;
    }
    void applied(const RasterState& r) noexcept {
        state=r;
        // Invalid requests must keep reaching GL/error reporting every draw.
        valid=r.viewport[2]>=0 && r.viewport[3]>=0 && r.scissor[2]>=0 && r.scissor[3]>=0 &&
            r.depthFunc>=0 && r.depthFunc<8;
    }
    void invalidate() noexcept {valid=false;}
private:
    RasterState state{};
    bool valid=false;
};
static_assert(sizeof(RasterShadow)<=80);
} // namespace twv::vgl
