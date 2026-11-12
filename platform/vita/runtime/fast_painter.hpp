#pragma once
// --vita-fast-<name>=off|on|verify: painter-side speed-ups with the same picture and the same GX
// command stream. Not renderer choices, so the play profile stays (main_vita.cpp); each has a
// play-profile default here. Plain integer C ABI between main_vita.cpp, this file, Aurora and the
// game patches.
//   gx-wake (patches/aurora/0020): the GX worker and the game thread wake each other at the same
//     points with a flag and a semaphore instead of std::atomic wait/notify. verify: timed sleeps,
//     lost wakeups counted.
//   static-dl (patches/aurora/0021): a display list by reference takes its id from a cache of the
//     map's answers (same pointer, size and map generation) instead of a lock and a map lookup.
//     verify: the map is asked too and its id written; different ids counted.
// GX worker speed-ups with the same draws, in the same list because they are selected the same way:
//   gen-mode (patches/aurora/0023): a genMode / numChans / numTexGens write that leaves the decoded
//     state as it is no longer marks the pipeline changed (J3D sends the three with every material,
//     and each invalidated the others' shadow registers). verify: handled as before; pipeline
//     rebuilds that only such writes asked for are counted, and those that gave another pipeline.
//   pipeline-info (gfx_vgl/backend.cpp twv_vita_pipeline_info_copy): the pipeline a lookup has just
//     returned for a config is not compared with that config a second time. verify: compared as
//     before, the comparisons that on would skip counted.
// Painter work the backend throws away:
//   unseen-map (patches/dusklight/0052): while the backend drops every draw into an offscreen
//     framebuffer (it has none yet), the minimap's and the map screen's polygons and lines are not
//     sent. verify counts the maps it would leave out and sends them.
// The geometry cache's shape:
//   wide-cache (gfx_vgl/geometry_cache.hpp set_ways): a packet is looked for in a group of 8 entries
//     instead of 4 (the same 512 entries), so a packet is no longer decoded every frame because
//     four others share its group. verify: as on, and every cache hit is decoded again by the
//     reference decoder and compared.
//   patient-cache (gfx_vgl/geometry_cache.hpp set_patient): a packet is copied into the cache the
//     second time its key is seen, not the first, and an entry survives one frame without use.
//     Packets whose key is new every frame (an animated model's two vertex buffers in turn, the
//     grass) are then no longer stored and thrown away every frame. verify: as on, and every
//     cache hit is decoded again by the reference decoder and compared.
// The GX worker's register decode:
//   reg-repeats (patches/aurora/0025): a TEV colour or konst write of the value last decoded for
//     its address and kind, and a light load of the words last decoded, do not go to their
//     handlers (J3D sends both with every material). verify: the handler still runs and the state
//     it writes is compared before and after.
//   draw-runs (patches/aurora/0026): quad or triangle-list draw commands that follow each other
//     with nothing changed between them (snow, rain, twilight squares, text: one quad per draw in
//     the game) are sent to the backend as one draw. verify: nothing is joined, and each draw that
//     would have joined is checked against Aurora's own change flags.
//   shadow-lists (patches/dusklight/0066): a simple (blob) shadow draws its volume three times,
//     and the volume's display list is eight triangle strips of 3 to 5 vertices, so one shadow is
//     24 draws on the GX worker with nothing changed between them: 358 of the 905 draws per frame
//     in the Ordon village overview (13 shadows; Vita3K census, 12 Nov). On: the volume is drawn
//     from a list in which Aurora's own load-time joiner (aurora::gx::dl::optimize, the one every
//     J3D shape goes through) has made the eight strips one indexed draw of the same 16 triangles
//     in the same order, called by reference. verify: the joined list is built and its size
//     checked, and the game's own list is drawn.
// The vertex decode of large packets:
//   decode-blocks (gfx_vgl/vertex.cpp build_gpu_vertices): a packet of more than 256 vertices is
//     decoded 256 vertices at a time by the same decoder, so each block's output stays in the
//     first-level cache across the decoder's passes. verify: the packet is also decoded in one
//     piece and compared; that result is used, differences are counted.
#include <cstdint>
#include <cstring>

namespace twv::vita {
enum : int { FastOff = 0, FastOn = 1, FastVerify = 2 };
constexpr const char* FastModeNames[] = {"off", "on", "verify"};
enum FastPainterOption : unsigned {
    FastGxWake, FastStaticDl, FastGenMode, FastPipelineInfo, FastUnseenMap, FastWideCache,
    FastDecodeBlocks, FastPatientCache, FastRegRepeats, FastDrawRuns, FastShadowLists, FastPainterOptionCount
};
constexpr const char* FastPainterOptionNames[FastPainterOptionCount] = {"gx-wake", "static-dl", "gen-mode",
                                                                        "pipeline-info", "unseen-map",
                                                                        "wide-cache", "decode-blocks",
                                                                        "patient-cache", "reg-repeats",
                                                                        "draw-runs", "shadow-lists"};
// Play-profile defaults: the first five on since the console session of 10 Nov
// (docs/benchmarks.csv). Vita3K verify in the six bench-v2 places found no lost wakeup, no
// different list id, no different pipeline and no differing draw digest; on the console the GX
// worker fell by 6.6 ms in Ordon centre, 8.0 in Kakariko and 13.6 in Hyrule Field with the five on
// together (and the pipeline memo). unseen-map only acts while the backend drops offscreen draws.
// wide-cache and decode-blocks: on since the console session of 10 Nov: verify compared 34,323
// cache hits and 572 large packets on the console without a difference; the worker fell by 1 to 1.7
// ms in Ordon centre. patient-cache: on since the console session of 11 Nov: verify compared 29,981
// cache hits on the console without a difference; Kakariko stores 16 packets per frame instead of
// 77 and its worker fell by 1.3 to 1.6 ms.
// reg-repeats, draw-runs and shadow-lists: off until a console run has verified them.
constexpr int FastPainterPlay[FastPainterOptionCount] = {FastOn, FastOn, FastOn, FastOn, FastOn, FastOn,
                                                         FastOn, FastOn, FastOff, FastOff, FastOff};

// "--vita-fast-<name>=off|on|verify" for a name above: 1 (choices[option] set), -1 for another
// value or a second time, 0 for any other token.
inline int fastPainterOption(const char* token, int (&choices)[FastPainterOptionCount]) {
    constexpr const char Prefix[] = "--vita-fast-";
    if (std::strncmp(token, Prefix, sizeof Prefix - 1) != 0) return 0;
    const char* name = token + sizeof Prefix - 1;
    for (unsigned option = 0; option < FastPainterOptionCount; ++option) {
        const std::size_t n = std::strlen(FastPainterOptionNames[option]);
        if (std::strncmp(name, FastPainterOptionNames[option], n) != 0 || name[n] != '=') continue;
        if (choices[option] >= 0) return -1;
        for (int mode = 0; mode < 3; ++mode)
            if (std::strcmp(name + n + 1, FastModeNames[mode]) == 0) {
                choices[option] = mode;
                return 1;
            }
        return -1;
    }
    return 0;
}
}  // namespace twv::vita

extern "C" {
// main_vita.cpp, before game_main: 0 off, 1 on, 2 verify. Returns 0 for an unknown option or mode.
int twv_vita_fast_painter_select(unsigned option, int mode);
int twv_vita_fast_painter_mode(unsigned option);
// patches/aurora/0020: read once when the GX worker starts.
int twv_gx_fast_wake_mode(void) noexcept;
// patches/aurora/0021: read at the first display list call by reference.
int twv_gx_fast_static_dl_mode(void) noexcept;
// patches/aurora/0023: read at the GX worker's first genMode / numChans / numTexGens write.
int twv_gx_fast_gen_mode(void) noexcept;
// patches/aurora/0025: read at the GX worker's first register write.
int twv_gx_fast_reg_repeats(void) noexcept;
// patches/aurora/0026: read at the GX worker's first draw.
int twv_gx_fast_draw_runs(void) noexcept;
// patches/dusklight/0066 (d_drawlist.cpp): the shadow-lists mode, and a count per shadow volume
// drawn: 0 joined list drawn, 1 the game's list drawn because no joined list could be made,
// 2 verify found a joined list of an unexpected size.
int twv_vita_fast_shadow_lists(void);
void twv_vita_shadow_list_note(int what);
// patches/dusklight/0052: 1 when the map's path is to be left out of this painter.
int twv_vita_unseen_map_skip(void);
// After every painter (actor_profile.cpp): receipts once in ReceiptFrames painted frames.
void twv_vita_fast_painter_frame(void);
}
