// --vita-fast-<name> (runtime/fast_painter.hpp): option parsing, mode selection, play defaults and the
// gx-wake receipts (patches/aurora/0020 feeds the counts; the mechanism is checked on the target by
// its verify mode).
#include "runtime/fast_painter.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

std::map<std::string, long long> events;
unsigned receiptGroups = 0;
namespace twv::vita {
void telemetryEvent(const char* name, long long value) {
    if (std::string(name) == "fast_gx_wake_mode") ++receiptGroups;
    events[name] = value;
}
}
#include "runtime/fast_painter.cpp"
// patches/aurora/0020's counts, after the file's weak declaration (as in the real link).
uint32_t fakeCounts[5] = {}, fakeStatic[4] = {};
extern "C" int twv_gx_fast_wake_stats(uint32_t* counts, unsigned n) noexcept {
    for (unsigned i = 0; i < n && i < 5; ++i) counts[i] = fakeCounts[i];
    return twv_gx_fast_wake_mode();
}
extern "C" int twv_gx_fast_static_dl_stats(uint32_t* counts, unsigned n) noexcept {
    for (unsigned i = 0; i < n && i < 4; ++i) counts[i] = fakeStatic[i];
    return twv_gx_fast_static_dl_mode();
}

int main() {
    using namespace twv::vita;
    int choices[FastPainterOptionCount];
    for (int& c : choices) c = -1;
    assert(fastPainterOption("--stage", choices) == 0);
    assert(fastPainterOption("--vita-fast-shadow-collect=on", choices) == 0);  // another option's prefix
    assert(fastPainterOption("--vita-fast-gx-wakex=on", choices) == 0);
    assert(fastPainterOption("--vita-fast-gx-wake=ON", choices) == -1 && choices[FastGxWake] == -1);
    assert(fastPainterOption("--vita-fast-gx-wake=", choices) == -1 && choices[FastGxWake] == -1);
    for (int mode = 0; mode < 3; ++mode) {
        choices[FastGxWake] = -1;
        const std::string token = std::string("--vita-fast-gx-wake=") + FastModeNames[mode];
        assert(fastPainterOption(token.c_str(), choices) == 1 && choices[FastGxWake] == mode);
        assert(fastPainterOption(token.c_str(), choices) == -1 && choices[FastGxWake] == mode);
    }
    for (int d : FastPainterPlay) assert(d >= FastOff && d <= FastVerify);

    assert(twv_vita_fast_painter_mode(FastGxWake) == FastOff && twv_gx_fast_wake_mode() == FastOff);
    assert(!twv_vita_fast_painter_select(FastPainterOptionCount, FastOn));
    assert(!twv_vita_fast_painter_select(FastGxWake, 3) && !twv_vita_fast_painter_select(FastGxWake, -1));
    assert(twv_vita_fast_painter_select(FastGxWake, FastVerify));
    assert(twv_vita_fast_painter_mode(FastGxWake) == FastVerify && twv_gx_fast_wake_mode() == FastVerify);
    assert(twv_vita_fast_painter_mode(FastPainterOptionCount) == 0);

    // 299 frames: no periodic group; a lost wakeup in verify reports at once, once.
    fakeCounts[0] = 10; fakeCounts[1] = 12;
    for (unsigned i = 0; i < 299; ++i) twv_vita_fast_painter_frame();
    assert(receiptGroups == 0);
    fakeCounts[4] = 1;
    twv_vita_fast_painter_frame();  // frame 300: periodic and lost together, one group
    assert(receiptGroups == 1 && events["fast_gx_wake_lost"] == 1 && events["fast_gx_wake_mode"] == 2);
    twv_vita_fast_painter_frame();
    assert(receiptGroups == 1);
    fakeCounts[4] = 2;
    twv_vita_fast_painter_frame();
    assert(receiptGroups == 2 && events["fast_gx_wake_lost"] == 2);
    assert(events["fast_gx_wake_worker_signals"] == 10 && events["fast_gx_wake_worker_sleeps"] == 12);
    // On: lost wakeups are not looked for between groups.
    assert(twv_vita_fast_painter_select(FastGxWake, FastOn));
    fakeCounts[4] = 3;
    twv_vita_fast_painter_frame();
    assert(receiptGroups == 2);
    // static-dl: parsed like gx-wake, its own mode; a verify mismatch reports at once.
    choices[FastStaticDl] = -1;
    assert(fastPainterOption("--vita-fast-static-dl=verify", choices) == 1 && choices[FastStaticDl] == FastVerify);
    assert(fastPainterOption("--vita-fast-static-dl=on", choices) == -1);
    assert(twv_vita_fast_painter_select(FastStaticDl, FastVerify) && twv_gx_fast_static_dl_mode() == FastVerify);
    assert(twv_gx_fast_wake_mode() == FastOn);
    fakeStatic[0] = 100; fakeStatic[1] = 7; fakeStatic[2] = 7; fakeStatic[3] = 1;
    twv_vita_fast_painter_frame();
    assert(receiptGroups == 3 && events["fast_static_dl_mismatches"] == 1 && events["fast_static_dl_mode"] == 2);
    assert(events["fast_static_dl_cached"] == 100 && events["fast_static_dl_map"] == 7);
    twv_vita_fast_painter_frame();
    assert(receiptGroups == 3);
    std::puts("fast_painter_test: ok");
    return 0;
}
