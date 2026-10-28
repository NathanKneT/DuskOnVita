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
#include <cstdint>
#include <cstring>

namespace twv::vita {
enum : int { FastOff = 0, FastOn = 1, FastVerify = 2 };
constexpr const char* FastModeNames[] = {"off", "on", "verify"};
enum FastPainterOption : unsigned { FastGxWake, FastStaticDl, FastPainterOptionCount };
constexpr const char* FastPainterOptionNames[FastPainterOptionCount] = {"gx-wake", "static-dl"};
// Play-profile defaults: off until verify has covered the six bench-v2 places (so far Ordon centre
// only: no lost wakeup, no different id; docs/PERFORMANCE.md).
constexpr int FastPainterPlay[FastPainterOptionCount] = {FastOff, FastOff};

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
// After every painter (actor_profile.cpp): receipts once in ReceiptFrames painted frames.
void twv_vita_fast_painter_frame(void);
}
