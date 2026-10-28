#pragma once
// --vita-fast-shadow-collect=off|on|verify (patches/dusklight/0018): real-time shadow ground polygon
// collection with the same receiver list, from an exact early stop (list full) and an exact
// pre-filter (psdRealCallBack's own light/plane tests before the KC vertices are built).
// verify: the original walk feeds the game, the fast walk a scratch list; lists that differ bit for
// bit are counted. Plain integer C ABI between main_vita.cpp, this file and the game patch.
#include <cstdint>
#include <cstring>

namespace twv::vita {
enum : int { FastShadowCollectOff = 0, FastShadowCollectOn = 1, FastShadowCollectVerify = 2 };
constexpr const char* FastShadowCollectNames[] = {"off", "on", "verify"};

// "--vita-fast-shadow-collect=off|on|verify": 1 (choice set), -1 for another value or a second
// time, 0 for any other token.
inline int fastShadowCollectOption(const char* token, int& choice) {
    constexpr const char Prefix[] = "--vita-fast-shadow-collect=";
    if (std::strncmp(token, Prefix, sizeof Prefix - 1) != 0) return 0;
    if (choice >= 0) return -1;
    for (int mode = 0; mode < 3; ++mode)
        if (std::strcmp(token + sizeof Prefix - 1, FastShadowCollectNames[mode]) == 0) {
            choice = mode;
            return 1;
        }
    return -1;
}

struct FastShadowCollectStats {
    std::uint64_t calls = 0, tris = 0, full = 0, verified = 0, mismatches = 0;
};
FastShadowCollectStats fastShadowCollectStats();
}

extern "C" {
// main_vita.cpp, before game_main: 0 off, 1 on, 2 verify. Returns 0 for any other value.
int twv_vita_fast_shadow_collect_select(int mode);
// patches/dusklight/0018, every realPolygonCheck: the mode, then the collection's result
// (triangles, list full, mismatch -1 not verified / 0 same / 1 different).
int twv_vita_fast_shadow_collect_mode(void);
void twv_vita_fast_shadow_collect_report(unsigned tris, unsigned full, int mismatch);
}
