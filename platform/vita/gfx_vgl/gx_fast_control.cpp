#include "gx_fast_api.h"
#include "vertex_decoder_control.hpp"
#include "runtime/telemetry.hpp"

#ifndef TWV_VITA_GX_FAST_DECODE
#define TWV_VITA_GX_FAST_DECODE 0
#endif

namespace {
twv::vgl::VertexDecoderControl choice{bool(TWV_VITA_GX_FAST_DECODE)};
unsigned failureReason=0;
unsigned long long failures=0;
}

extern "C" int twv_vita_gx_fast_select(int enabled) noexcept {
    return choice.select(enabled);
}
extern "C" int twv_vita_gx_fast_enabled() noexcept {
    choice.begin();
    return choice.enabled();
}
extern "C" void twv_vita_gx_fast_disable(unsigned reason) noexcept {
    choice.fail();
    if(!failures)failureReason=reason;
    ++failures;
    if(failures==1) {
        twv::vita::telemetryEvent("gx_fast_failure_reason",reason);
        twv::vita::telemetryFlush();
    }
}
extern "C" void twv_vita_gx_fast_report() noexcept {
    twv::vita::telemetryEvent("gx_fast_compiled",choice.compiled());
    twv::vita::telemetryEvent("gx_fast_explicit",choice.selected());
    twv::vita::telemetryEvent("gx_fast_enabled",choice.enabled());
    twv::vita::telemetryEvent("gx_fast_failures",failures);
    twv::vita::telemetryEvent("gx_fast_first_failure_reason",failureReason);
}
