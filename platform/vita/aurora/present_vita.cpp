// The fixed-size Vita GPU owns presentation; window.cpp must not create a Dawn
// surface or swapchain. Original VI logical dimensions remain in the GX state.
#include "runtime/telemetry.hpp"
#include <cstdint>
namespace aurora::webgpu {
bool refresh_surface(bool) {
    twv::vita::telemetryText("present_surface", "fixed Vita surface; no WebGPU surface");
    return false;
}
void resize_swapchain(uint32_t w,uint32_t h,uint32_t native_w,uint32_t native_h,bool) {
    twv::vita::telemetryEvent("window_fb_width",w);
    twv::vita::telemetryEvent("window_fb_height",h);
    twv::vita::telemetryEvent("window_native_width",native_w);
    twv::vita::telemetryEvent("window_native_height",native_h);
}
}
