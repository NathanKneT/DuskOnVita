// --vita-fast-shadow-collect (fast_shadow_collect.hpp). Game thread only: the selection happens
// before game_main, every report comes from the game's draw pass.
#include "runtime/fast_shadow_collect.hpp"
#include "runtime/telemetry.hpp"

namespace {
int sMode = twv::vita::FastShadowCollectOff;
twv::vita::FastShadowCollectStats sStats;

void receipts() {
    twv::vita::telemetryEvent("shadow_collect_calls", (long long)sStats.calls);
    twv::vita::telemetryEvent("shadow_collect_tris", (long long)sStats.tris);
    twv::vita::telemetryEvent("shadow_collect_full_lists", (long long)sStats.full);
    twv::vita::telemetryEvent("shadow_collect_verified", (long long)sStats.verified);
    twv::vita::telemetryEvent("shadow_collect_mismatches", (long long)sStats.mismatches);
}
}

namespace twv::vita {
FastShadowCollectStats fastShadowCollectStats() { return sStats; }
}

extern "C" int twv_vita_fast_shadow_collect_select(int mode) {
    if (mode < twv::vita::FastShadowCollectOff || mode > twv::vita::FastShadowCollectVerify) return 0;
    sMode = mode;
    return 1;
}

extern "C" int twv_vita_fast_shadow_collect_mode(void) { return sMode; }

extern "C" void twv_vita_fast_shadow_collect_report(unsigned tris, unsigned full, int mismatch) {
    ++sStats.calls;
    sStats.tris += tris;
    sStats.full += full != 0;
    bool now = sStats.calls % 900 == 0; // about 15 s with two shadows per tick
    if (mismatch >= 0) {
        ++sStats.verified;
        if (mismatch) {
            ++sStats.mismatches;
            now = now || sStats.mismatches <= 4;
        }
    }
    if (now) receipts();
}
