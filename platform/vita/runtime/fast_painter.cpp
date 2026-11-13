// --vita-fast-<name> (fast_painter.hpp). The selection happens on the game thread before game_main;
// the frame hook runs on the game thread after each painter.
#include "runtime/fast_painter.hpp"
#include "runtime/telemetry.hpp"

#include <cstdio>

// patches/aurora/0020 (fifo.cpp): the wake mode in use and its counts (worker signals, worker sleeps,
// waiter signals, waiter sleeps, lost wakeups). Weak: absent in a build without the GX worker.
extern "C" int twv_gx_fast_wake_stats(uint32_t* counts, unsigned n) noexcept __attribute__((weak));
// patches/aurora/0021 (GXDispList.cpp): the static list cache's mode and counts (calls from the cache,
// map lookups, verified calls, mismatches).
extern "C" int twv_gx_fast_static_dl_stats(uint32_t* counts, unsigned n) noexcept __attribute__((weak));

// patches/aurora/0023 (regs.cpp): the gen-mode switch's mode and counts (writes short-circuited,
// verify candidates, decode mismatches, pipeline rebuilds checked, rebuilds that gave another pipeline).
extern "C" int twv_gx_fast_gen_mode_stats(uint32_t* counts, unsigned n) noexcept __attribute__((weak));

// patches/aurora/0025 (regs.cpp): the reg-repeats switch's mode and counts (BP writes not decoded,
// light loads not decoded, BP mismatches, light mismatches).
extern "C" int twv_gx_fast_reg_repeats_stats(uint32_t* counts, unsigned n) noexcept __attribute__((weak));

// patches/aurora/0026 (command_processor.cpp): the draw-runs switch's mode and counts (runs, draws
// joined, verify candidates, verify mismatches).
extern "C" int twv_gx_fast_draw_runs_stats(uint32_t* counts, unsigned n) noexcept __attribute__((weak));

// patches/aurora/0027 (command_processor.cpp, measurement only): what ended each look ahead (8
// reasons for runs, 8 for draws that stayed alone), then the runs by size (quads, triangles).
extern "C" void twv_gx_fast_draw_runs_census(uint32_t* counts, unsigned n) noexcept __attribute__((weak));

// gfx_vgl/backend.cpp: 1 while the backend drops every draw into an offscreen framebuffer.
extern "C" int twv_vita_offscreen_draws_dropped(void) __attribute__((weak));

namespace {
uint32_t sUnseenMaps = 0; // maps left out (on) or that would have been (verify)
int sModes[twv::vita::FastPainterOptionCount] = {};
constexpr unsigned ReceiptFrames = 300;
unsigned sFrames = 0;
uint32_t sLostReported = 0, sMismatchReported = 0, sGenModeReported = 0, sRegRepeatsReported = 0,
         sDrawRunsReported = 0;

uint32_t sShadowLists[3]; // patches/dusklight/0066: joined, plain, verify mismatches
void receipts() {
    if (sModes[twv::vita::FastUnseenMap] != twv::vita::FastOff) {
        twv::vita::telemetryEvent("fast_unseen_map_mode", sModes[twv::vita::FastUnseenMap]);
        twv::vita::telemetryEvent("fast_unseen_map_maps", sUnseenMaps);
    }
    if (twv_gx_fast_gen_mode_stats) {
        uint32_t g[5] = {};
        const int mode = twv_gx_fast_gen_mode_stats(g, 5);
        twv::vita::telemetryEvent("fast_gen_mode_mode", mode);
        twv::vita::telemetryEvent("fast_gen_mode_short", g[0]);
        twv::vita::telemetryEvent("fast_gen_mode_candidates", g[1]);
        twv::vita::telemetryEvent("fast_gen_mode_decode_mismatches", g[2]);
        twv::vita::telemetryEvent("fast_gen_mode_rebuilds_checked", g[3]);
        twv::vita::telemetryEvent("fast_gen_mode_pipeline_mismatches", g[4]);
        sGenModeReported = g[2] + g[4];
    }
    if (sModes[twv::vita::FastShadowLists] != twv::vita::FastOff) {
        twv::vita::telemetryEvent("fast_shadow_lists_mode", sModes[twv::vita::FastShadowLists]);
        twv::vita::telemetryEvent("fast_shadow_lists_joined", sShadowLists[0]);
        twv::vita::telemetryEvent("fast_shadow_lists_plain", sShadowLists[1]);
        twv::vita::telemetryEvent("fast_shadow_lists_mismatches", sShadowLists[2]);
    }
    if (twv_gx_fast_draw_runs_stats && sModes[twv::vita::FastDrawRuns] != twv::vita::FastOff) {
        uint32_t d[4] = {};
        const int mode = twv_gx_fast_draw_runs_stats(d, 4);
        twv::vita::telemetryEvent("fast_draw_runs_mode", mode);
        twv::vita::telemetryEvent("fast_draw_runs_runs", d[0]);
        twv::vita::telemetryEvent("fast_draw_runs_joined", d[1]);
        twv::vita::telemetryEvent("fast_draw_runs_candidates", d[2]);
        twv::vita::telemetryEvent("fast_draw_runs_mismatches", d[3]);
        sDrawRunsReported = d[3];
        if (twv_gx_fast_draw_runs_census) {
            uint32_t c[24] = {};
            twv_gx_fast_draw_runs_census(c, 24);
            // Three lines: a text receipt holds 254 characters.
            static const char* const names[3] = {"fast_draw_runs_ends_run", "fast_draw_runs_ends_alone",
                                                 "fast_draw_runs_sizes"};
            static const char* const formats[3] = {
                "avail=%lu;bp=%lu;xf=%lu;cp=%lu;draw=%lu;other=%lu;limit=%lu;indexed=%lu",
                "avail=%lu;bp=%lu;xf=%lu;cp=%lu;draw=%lu;other=%lu;limit=%lu;indexed=%lu",
                "q4=%lu;q16=%lu;q64=%lu;qmore=%lu;t4=%lu;t16=%lu;t64=%lu;tmore=%lu"};
            for (unsigned g = 0; g < 3; ++g) {
                const uint32_t* v = c + g * 8;
                char text[176];
                std::snprintf(text, sizeof text, formats[g], (unsigned long)v[0], (unsigned long)v[1],
                              (unsigned long)v[2], (unsigned long)v[3], (unsigned long)v[4], (unsigned long)v[5],
                              (unsigned long)v[6], (unsigned long)v[7]);
                twv::vita::telemetryText(names[g], text);
            }
        }
    }
    if (twv_gx_fast_reg_repeats_stats && sModes[twv::vita::FastRegRepeats] != twv::vita::FastOff) {
        uint32_t r[4] = {};
        const int mode = twv_gx_fast_reg_repeats_stats(r, 4);
        twv::vita::telemetryEvent("fast_reg_repeats_mode", mode);
        twv::vita::telemetryEvent("fast_reg_repeats_bp", r[0]);
        twv::vita::telemetryEvent("fast_reg_repeats_lights", r[1]);
        twv::vita::telemetryEvent("fast_reg_repeats_bp_mismatches", r[2]);
        twv::vita::telemetryEvent("fast_reg_repeats_light_mismatches", r[3]);
        sRegRepeatsReported = r[2] + r[3];
    }
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
extern "C" int twv_gx_fast_gen_mode(void) noexcept {return sModes[twv::vita::FastGenMode];}
extern "C" int twv_gx_fast_reg_repeats(void) noexcept {return sModes[twv::vita::FastRegRepeats];}
extern "C" int twv_gx_fast_draw_runs(void) noexcept {return sModes[twv::vita::FastDrawRuns];}
// patches/dusklight/0066: the shadow-lists switch and its counts.
extern "C" int twv_vita_fast_shadow_lists(void) {return sModes[twv::vita::FastShadowLists];}
extern "C" void twv_vita_shadow_list_note(int what) {
    if (what >= 0 && what < 3) ++sShadowLists[what];
}
extern "C" int twv_vita_unseen_map_skip(void) {
    const int mode = sModes[twv::vita::FastUnseenMap];
    if (mode == twv::vita::FastOff || !twv_vita_offscreen_draws_dropped || !twv_vita_offscreen_draws_dropped())
        return 0;
    ++sUnseenMaps;
    return mode == twv::vita::FastOn;
}

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
    if (!now && twv_gx_fast_gen_mode_stats && sModes[twv::vita::FastGenMode] == twv::vita::FastVerify) {
        uint32_t g[5] = {};
        twv_gx_fast_gen_mode_stats(g, 5);
        now = g[2] + g[4] != sGenModeReported;
    }
    if (!now && twv_gx_fast_draw_runs_stats && sModes[twv::vita::FastDrawRuns] == twv::vita::FastVerify) {
        uint32_t d[4] = {};
        twv_gx_fast_draw_runs_stats(d, 4);
        now = d[3] != sDrawRunsReported;
    }
    if (!now && twv_gx_fast_reg_repeats_stats && sModes[twv::vita::FastRegRepeats] == twv::vita::FastVerify) {
        uint32_t r[4] = {};
        twv_gx_fast_reg_repeats_stats(r, 4);
        now = r[2] + r[3] != sRegRepeatsReported;
    }
    if (now) receipts();
}
