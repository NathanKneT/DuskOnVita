// --vita-fast-<name> (fast_painter.hpp). The selection happens on the game thread before game_main;
// the frame hook runs on the game thread after each painter.
#include "runtime/fast_painter.hpp"
#include "runtime/telemetry.hpp"

// patches/aurora/0020 (fifo.cpp): the wake mode in use and its counts (worker signals, worker sleeps,
// waiter signals, waiter sleeps, lost wakeups). Weak: absent in a build without the GX worker.
extern "C" int twv_gx_fast_wake_stats(uint32_t* counts, unsigned n) noexcept __attribute__((weak));
// patches/aurora/0021 (GXDispList.cpp): the static list cache's mode and counts (calls from the cache,
// map lookups, verified calls, mismatches).
extern "C" int twv_gx_fast_static_dl_stats(uint32_t* counts, unsigned n) noexcept __attribute__((weak));

namespace {
int sModes[twv::vita::FastPainterOptionCount] = {};
constexpr unsigned ReceiptFrames = 300;
unsigned sFrames = 0;
uint32_t sLostReported = 0, sMismatchReported = 0;

void receipts() {
    if (twv_gx_fast_static_dl_stats) {
        uint32_t s[4] = {};
        const int mode = twv_gx_fast_static_dl_stats(s, 4);
        twv::vita::telemetryEvent("fast_static_dl_mode", mode);
        twv::vita::telemetryEvent("fast_static_dl_cached", s[0]);
        twv::vita::telemetryEvent("fast_static_dl_map", s[1]);
        twv::vita::telemetryEvent("fast_static_dl_verified", s[2]);
        twv::vita::telemetryEvent("fast_static_dl_mismatches", s[3]);
        sMismatchReported = s[3];
    }
    if (!twv_gx_fast_wake_stats) return;
    uint32_t c[5] = {};
    const int mode = twv_gx_fast_wake_stats(c, 5);
    twv::vita::telemetryEvent("fast_gx_wake_mode", mode);
    twv::vita::telemetryEvent("fast_gx_wake_worker_signals", c[0]);
    twv::vita::telemetryEvent("fast_gx_wake_worker_sleeps", c[1]);
    twv::vita::telemetryEvent("fast_gx_wake_waiter_signals", c[2]);
    twv::vita::telemetryEvent("fast_gx_wake_waiter_sleeps", c[3]);
    twv::vita::telemetryEvent("fast_gx_wake_lost", c[4]);
    sLostReported = c[4];
}
}  // namespace

extern "C" int twv_vita_fast_painter_select(unsigned option, int mode) {
    if (option >= twv::vita::FastPainterOptionCount || mode < twv::vita::FastOff || mode > twv::vita::FastVerify)
        return 0;
    sModes[option] = mode;
    return 1;
}

extern "C" int twv_vita_fast_painter_mode(unsigned option) {
    return option < twv::vita::FastPainterOptionCount ? sModes[option] : 0;
}

extern "C" int twv_gx_fast_wake_mode(void) noexcept {return sModes[twv::vita::FastGxWake];}
extern "C" int twv_gx_fast_static_dl_mode(void) noexcept {return sModes[twv::vita::FastStaticDl];}

extern "C" void twv_vita_fast_painter_frame(void) {
    bool now = ++sFrames % ReceiptFrames == 0;
    // A lost wakeup or a static list mismatch reports at once (verify), not only with the group.
    if (!now && twv_gx_fast_wake_stats && sModes[twv::vita::FastGxWake] == twv::vita::FastVerify) {
        uint32_t c[5] = {};
        twv_gx_fast_wake_stats(c, 5);
        now = c[4] != sLostReported;
    }
    if (!now && twv_gx_fast_static_dl_stats && sModes[twv::vita::FastStaticDl] == twv::vita::FastVerify) {
        uint32_t s[4] = {};
        twv_gx_fast_static_dl_stats(s, 4);
        now = s[3] != sMismatchReported;
    }
    if (now) receipts();
}
