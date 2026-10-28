// --vita-fast-<name> (fast_actors.hpp). Game thread only: the selection happens before game_main,
// every count comes from actor execute.
#include "runtime/fast_actors.hpp"
#include "runtime/telemetry.hpp"
#include <cstdio>

namespace {
int sMode[twv::vita::FastActorSwitchCount] = {};
twv::vita::FastActorStats sStats[twv::vita::FastActorSwitchCount];

void receipts(int which) {
    const twv::vita::FastActorStats& s = sStats[which];
    char name[64];
    const char* base = twv::vita::FastActorSwitchNames[which];
    std::snprintf(name, sizeof name, "fast_%s_calls", base);
    twv::vita::telemetryEvent(name, (long long)s.calls);
    std::snprintf(name, sizeof name, "fast_%s_fast", base);
    twv::vita::telemetryEvent(name, (long long)s.fast);
    std::snprintf(name, sizeof name, "fast_%s_walks", base);
    twv::vita::telemetryEvent(name, (long long)s.walks);
    std::snprintf(name, sizeof name, "fast_%s_verified", base);
    twv::vita::telemetryEvent(name, (long long)s.verified);
    std::snprintf(name, sizeof name, "fast_%s_mismatches", base);
    twv::vita::telemetryEvent(name, (long long)s.mismatches);
}
}

namespace twv::vita {
FastActorStats fastActorStats(int which) {
    return which >= 0 && which < FastActorSwitchCount ? sStats[which] : FastActorStats{};
}
}

// runtime/actor_profile.cpp, once per receipt window (full receipts only): every switch's totals,
// so short runs report uses that never reach the periodic count.
extern "C" void twv_vita_fast_actor_publish(void) {
    for (int which = 0; which < twv::vita::FastActorSwitchCount; ++which)
        if (sStats[which].calls) receipts(which);
}

extern "C" int twv_vita_fast_actor_select(int which, int mode) {
    if (which < 0 || which >= twv::vita::FastActorSwitchCount) return 0;
    if (mode < twv::vita::FastActorOff || mode > twv::vita::FastActorVerify) return 0;
    sMode[which] = mode;
    return 1;
}

extern "C" int twv_vita_fast_actor_mode(int which) {
    return which >= 0 && which < twv::vita::FastActorSwitchCount ? sMode[which] : 0;
}

extern "C" void twv_vita_fast_actor_count(int which, int fast, int mismatch) {
    if (which < 0 || which >= twv::vita::FastActorSwitchCount) return;
    twv::vita::FastActorStats& s = sStats[which];
    ++s.calls;
    s.fast += fast != 0;
    s.walks += fast == 2;
    bool now = (s.calls & 8191) == 0; // seconds apart at dozens to hundreds of uses per tick
    if (mismatch >= 0) {
        ++s.verified;
        if (mismatch) {
            ++s.mismatches;
            now = now || s.mismatches <= 4;
        }
    }
    if (now) receipts(which);
}
