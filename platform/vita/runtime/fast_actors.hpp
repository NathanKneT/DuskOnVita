#pragma once
// --vita-fast-<name>=off|on|verify: exact fast paths inside actor execute (patches/dusklight/0031 on).
// Each switch replaces work whose result the game uses with a cheaper path that produces the same
// result bit for bit; verify runs the original for the game and the fast path beside it, and counts
// the calls whose results differ. Not renderer choices, so the play profile stays.
// Plain integer C ABI between main_vita.cpp, this file and the game patches.
#include <cstdint>
#include <cstring>

namespace twv::vita {
enum : int { FastActorOff = 0, FastActorOn = 1, FastActorVerify = 2 };
constexpr const char* FastActorModeNames[] = {"off", "on", "verify"};

// Switch numbers are part of the patches' ABI (each patch names its own).
//  obstacle (0031): the passers' path obstacle search keeps its candidate actors while no node list
//    changed, instead of walking every process per passer.
//  search (0032): every process search (fpcLyIt_AllJudge) hands the judge the processes of a kept
//    walk while no node list changed, and goes on as the original loop once a judge changes one.
//  bg-list (0033): terrain queries visit the used check elements from a bitmap kept where elements
//    are registered and released, instead of testing all 256 (walls: three times 256).
//  flow-bind (0034): a message flow binding is kept per resource and group while no message
//    resource was set since, instead of parsing the whole BMG on every dMsgFlow_c::init (doors).
enum FastActorSwitch : int { FastObstacle = 0, FastSearch = 1, FastBgList = 2, FastFlowBind = 3,
                             FastActorSwitchCount };
constexpr const char* FastActorSwitchNames[FastActorSwitchCount] = {"obstacle", "search", "bg-list",
                                                                    "flow-bind"};

// "--vita-fast-<name>=off|on|verify" for a known name: 1 (choices[name] set), -1 for another value
// or a second time, 0 for any other token.
inline int fastActorOption(const char* token, int (&choices)[FastActorSwitchCount]) {
    constexpr const char Prefix[] = "--vita-fast-";
    if (std::strncmp(token, Prefix, sizeof Prefix - 1) != 0) return 0;
    const char* rest = token + sizeof Prefix - 1;
    for (int which = 0; which < FastActorSwitchCount; ++which) {
        const std::size_t n = std::strlen(FastActorSwitchNames[which]);
        if (std::strncmp(rest, FastActorSwitchNames[which], n) != 0 || (rest[n] != '=' && rest[n] != '\0'))
            continue;
        if (choices[which] >= 0 || rest[n] == '\0') return -1;
        for (int mode = 0; mode < 3; ++mode)
            if (std::strcmp(rest + n + 1, FastActorModeNames[mode]) == 0) {
                choices[which] = mode;
                return 1;
            }
        return -1;
    }
    return 0;
}

struct FastActorStats {
    // calls: every use; fast: answered by the fast path; walks: fast answers that rebuilt their
    // kept state first; verified/mismatches: verify mode comparisons.
    std::uint64_t calls = 0, fast = 0, walks = 0, verified = 0, mismatches = 0;
};
FastActorStats fastActorStats(int which);
}

extern "C" {
// main_vita.cpp, before game_main: switch, 0 off, 1 on, 2 verify. Returns 0 for any other value.
int twv_vita_fast_actor_select(int which, int mode);
// The game patches, per use: the switch's mode, then what happened (fast 0 original path, 1 fast,
// 2 fast after rebuilding; mismatch -1 not verified, 0 same, 1 different).
int twv_vita_fast_actor_mode(int which);
void twv_vita_fast_actor_count(int which, int fast, int mismatch);
// The actor profile's receipt window: every used switch's totals.
void twv_vita_fast_actor_publish(void);
}
