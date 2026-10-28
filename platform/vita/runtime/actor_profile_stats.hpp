#pragma once
// Main-thread actor profile (actor_profile.cpp): per-window accumulators, plain types only, no game
// headers, so the host tests can check binning, top-N choice and receipt text.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace twv::vita::actprof {

// Process profile names (fpcNm_*, s16) index the type table; anything outside lands in the last slot.
constexpr unsigned TypeSlots = 1024;
inline unsigned type_slot(int profname) {
    return profname >= 0 && profname < int(TypeSlots) - 1 ? unsigned(profname) : TypeSlots - 1;
}

// Distance from the camera eye of actors that drew, in game units (stage coordinates; Link is about
// 140 tall): <1000, 1000-3000, 3000-6000, >=6000.
constexpr unsigned DistanceBins = 4;
constexpr float DistanceBounds[DistanceBins - 1] = {1000.0f, 3000.0f, 6000.0f};
inline unsigned distance_bin(float d) {
    unsigned b = 0;
    while (b < DistanceBins - 1 && !(d < DistanceBounds[b])) ++b;
    return b;
}

// J3DModel parts (patches/dusklight/0020 numbering) and where they ran. Draw parts are split by tick:
// "nonfinal" is a tick whose draw lists the next tick of the same frame resets before painting.
enum Part : unsigned { PartCalc, PartMaterial, PartDiff, PartEntry, PartView, PartCount };
constexpr const char* PartNames[PartCount] = {"calc", "material", "diff", "entry", "view"};
enum Where : unsigned { WhereExecute, WhereDrawFinal, WhereDrawNonfinal, WherePresent, WherePainter,
                        WhereOther, WhereCount };
constexpr const char* WhereNames[WhereCount] = {"execute", "draw_final", "draw_nonfinal", "present",
                                                "painter", "other"};

struct TypeStats {
    uint32_t execUs = 0, drawUs = 0;
    uint32_t execCalls = 0, drew = 0, culledFrustum = 0, culledFar = 0, skipped = 0;
    int argument = 0;
    bool actor = false;
};

struct PartStats {
    uint64_t us[PartCount] = {};
    uint32_t calls[PartCount] = {};
    void add(const PartStats& o) {
        for (unsigned p = 0; p < PartCount; ++p) {us[p] += o.us[p]; calls[p] += o.calls[p];}
    }
};

// Indices of the n types with the largest execute + draw time, largest first (ties: lower slot first).
inline std::size_t top_types(const TypeStats* types, std::size_t count, unsigned* out, std::size_t n) {
    std::size_t used = 0;
    auto total = [&](unsigned i) {return uint64_t(types[i].execUs) + types[i].drawUs;};
    for (unsigned i = 0; i < count; ++i) {
        if (!types[i].execCalls && !types[i].drew && !types[i].culledFrustum && !types[i].culledFar &&
            !types[i].skipped) continue;
        std::size_t pos = used < n ? used : n;
        while (pos > 0 && total(out[pos - 1]) < total(i)) --pos;
        if (pos >= n) continue;
        const std::size_t last = used < n ? used : n - 1;
        for (std::size_t k = last; k > pos; --k) out[k] = out[k - 1];
        out[pos] = i;
        if (used < n) ++used;
    }
    return used;
}

// One receipt line per type: name, profile, execute/draw us, and the counts over the sampled ticks.
inline int type_text(char* out, std::size_t size, const char* name, unsigned slot, const TypeStats& t) {
    return std::snprintf(out, size, "n=%s;p=%u;a=%d;ex=%lu;dw=%lu;ce=%lu;cd=%lu;cf=%lu;cr=%lu;cs=%lu",
        name, slot, t.actor ? 1 : 0, (unsigned long)t.execUs, (unsigned long)t.drawUs,
        (unsigned long)t.execCalls, (unsigned long)t.drew, (unsigned long)t.culledFrustum,
        (unsigned long)t.culledFar, (unsigned long)t.skipped);
}

inline int part_text(char* out, std::size_t size, const char* where, const PartStats& s) {
    return std::snprintf(out, size, "w=%s;us=%llu,%llu,%llu,%llu,%llu;n=%lu,%lu,%lu,%lu,%lu", where,
        (unsigned long long)s.us[0], (unsigned long long)s.us[1], (unsigned long long)s.us[2],
        (unsigned long long)s.us[3], (unsigned long long)s.us[4], (unsigned long)s.calls[0],
        (unsigned long)s.calls[1], (unsigned long)s.calls[2], (unsigned long)s.calls[3],
        (unsigned long)s.calls[4]);
}

// Execute detail (sub frames): inclusive time of selected calls inside actor execute, charged to the
// executing process's profile. A part nested in itself counts once (the outermost call); different
// parts nest (acch holds ground/wall/roof, j3d_calc holds joint_tree/weight_env): do not sum them.
enum Sub : unsigned {
    SubJ3dCalc, SubJointTree, SubWeightEnv, SubAcch, SubGround, SubLine, SubSph, SubWall, SubRoof,
    SubSearchAll, SubSearchActor, SubMorfPlay, SubCreature, SubCcSet, SubMsgFlow, SubFlowBind,
    SubLinkPre, SubLinkAnime, SubLinkProc, SubLinkBg, SubLinkGround, SubLinkMatrix, SubLinkTail,
    SubMeterDraw, SubMeterMoves, SubMeterMap, SubMeterSub, SubCameraRun, SubCameraView, SubCount
};
constexpr const char* SubNames[SubCount] = {
    "j3d_calc", "joint_tree", "weight_env", "acch", "ground", "line", "sph", "wall", "roof",
    "search_all", "search_actor", "morf_play", "z2_creature", "cc_set", "msg_flow", "flow_bind",
    "link_pre", "link_anime", "link_proc", "link_bg", "link_ground", "link_matrix", "link_tail",
    "meter_draw", "meter_moves", "meter_map", "meter_sub", "camera_run", "camera_view"};

// One receipt line of a type's execute detail: "p=<profile>;<sub>:<us>/<calls>,..." for the parts
// from *next on with calls, until the line is full. Returns the length written (0: nothing left);
// *next moves past the parts written.
inline int sub_text(char* out, std::size_t size, unsigned slot, const uint32_t* us, const uint32_t* calls,
                    unsigned* next) {
    int n = std::snprintf(out, size, "p=%u;", slot);
    bool any = false;
    unsigned k = *next;
    for (; k < SubCount; ++k) {
        if (!calls[k]) continue;
        char item[40];
        const int m = std::snprintf(item, sizeof item, "%s%u:%lu/%lu", any ? "," : "", k, (unsigned long)us[k],
                                    (unsigned long)calls[k]);
        if (std::size_t(n + m) >= size || n + m > 200) break;
        std::memcpy(out + n, item, std::size_t(m) + 1);
        n += m;
        any = true;
    }
    *next = k;
    return any ? n : 0;
}

// Sampling: one frame in Period is an actor frame (per-process timing), another a J3D frame (model
// part timing), another a sub frame (execute detail, sub_text); the rest pay flag tests only. Frame
// numbers count presented frames from 1.
constexpr unsigned Period = 8;
enum Mode : unsigned { ModeNone, ModeActors, ModeJ3D, ModeSub };
inline Mode frame_mode(uint64_t frame) {
    const unsigned r = unsigned(frame % Period);
    return r == 1 ? ModeActors : r == 5 ? ModeJ3D : r == 3 ? ModeSub : ModeNone;
}

}  // namespace twv::vita::actprof
