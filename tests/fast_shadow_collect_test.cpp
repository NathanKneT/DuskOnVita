// --vita-fast-shadow-collect: option parsing, mode selection and the counters the game patch feeds
// (patches/dusklight/0018). The walk itself is checked on the target by the verify mode.
#include "runtime/fast_shadow_collect.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

std::map<std::string, long long> events;
unsigned receiptGroups = 0;
namespace twv::vita {
void telemetryEvent(const char* name, long long value) {
    if (std::string(name) == "shadow_collect_calls") ++receiptGroups;
    events[name] = value;
}
}
#include "runtime/fast_shadow_collect.cpp"

int main() {
    using namespace twv::vita;
    int choice = -1;
    assert(fastShadowCollectOption("--vita-fast-shadow-collectx=on", choice) == 0 && choice == -1);
    assert(fastShadowCollectOption("--stage", choice) == 0 && choice == -1);
    assert(fastShadowCollectOption("--vita-fast-shadow-collect=ON", choice) == -1 && choice == -1);
    assert(fastShadowCollectOption("--vita-fast-shadow-collect=", choice) == -1 && choice == -1);
    assert(fastShadowCollectOption("--vita-fast-shadow-collect=onx", choice) == -1 && choice == -1);
    for (int mode = 0; mode < 3; ++mode) {
        choice = -1;
        const std::string token = std::string("--vita-fast-shadow-collect=") + FastShadowCollectNames[mode];
        assert(fastShadowCollectOption(token.c_str(), choice) == 1 && choice == mode);
        assert(fastShadowCollectOption(token.c_str(), choice) == -1 && choice == mode);
    }
    assert(twv_vita_fast_shadow_collect_mode() == FastShadowCollectOff);
    assert(!twv_vita_fast_shadow_collect_select(3) && !twv_vita_fast_shadow_collect_select(-1));
    assert(twv_vita_fast_shadow_collect_select(FastShadowCollectVerify));
    assert(twv_vita_fast_shadow_collect_mode() == FastShadowCollectVerify);
    // 899 reports: no periodic group yet; a mismatch reports at once (first four only).
    for (unsigned i = 0; i < 899; ++i) twv_vita_fast_shadow_collect_report(i % 257, i % 257 == 256, i % 2 ? 0 : -1);
    assert(receiptGroups == 0);
    twv_vita_fast_shadow_collect_report(256, 1, 1);
    assert(receiptGroups == 1 && events.at("shadow_collect_calls") == 900 && events.at("shadow_collect_mismatches") == 1);
    auto stats = fastShadowCollectStats();
    unsigned long long tris = 256;
    for (unsigned i = 0; i < 899; ++i) tris += i % 257;
    assert(stats.calls == 900 && stats.tris == tris && stats.full == 3 + 1 && stats.verified == 449 + 1);
    for (unsigned i = 0; i < 5; ++i) twv_vita_fast_shadow_collect_report(0, 0, 1);
    assert(receiptGroups == 4 && fastShadowCollectStats().mismatches == 6);
    std::puts("fast_shadow_collect_test: option, mode selection and verify counters passed");
    return 0;
}
