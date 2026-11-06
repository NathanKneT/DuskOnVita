// Visual-only speed-ups and the game-state digest (visual_skips.hpp). Game thread only.
#include "runtime/visual_skips.hpp"
#include "runtime/telemetry.hpp"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_tag.h"
#include "SSystem/SComponent/c_tag.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
int twv_skip_hidden_draw_mode = 0;
int twv_skip_offscreen_anim_mode = 0;
int twv_rest_creatures_mode = 0;
int twv_crowd_level = 0;
unsigned twv_vita_tick_serial = 0;
// patches/dusklight/0040 and 0044 (c_math.cpp): the two random generators' states.
void twv_cM_rnd_state(int* out);
void twv_cM_rnd2_state(int* out);
}

namespace {
twv::vita::VisualSkipStats sStats;
bool sDigest = false;
constexpr unsigned ReceiptTicks = 300;

// FNV-1a over 32-bit words.
struct Hash {
    uint32_t h = 2166136261u;
    void word(uint32_t w) {
        for (int i = 0; i < 4; ++i) {
            h ^= (w >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    }
    void f(float v) {uint32_t u; std::memcpy(&u, &v, 4); word(u);}
    void vec(const cXyz& v) {f(v.x); f(v.y); f(v.z);}
    void ang(const csXyz& a) {word(uint16_t(a.x) | uint32_t(uint16_t(a.y)) << 16); word(uint16_t(a.z));}
    void place(const actor_place& p) {vec(p.pos); ang(p.angle); word(uint8_t(p.roomNo));}
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) {h ^= b[i]; h *= 16777619u;}
    }
};

// Every actor in the actor queue's order: its process ID and name, and the fields the game's logic
// keeps about it between ticks (pointers and the draw-side light state left out).
uint32_t actors_hash(unsigned& count) {
    Hash h;
    count = 0;
    for (node_class* node = g_fopAcTg_Queue.mpHead; node != nullptr; node = node->mpNextNode) {
        const auto* a = static_cast<const fopAc_ac_c*>(reinterpret_cast<create_tag_class*>(node)->mpTagData);
        if (!a) continue;
        ++count;
        const auto* proc = reinterpret_cast<const base_process_class*>(a);
        h.word(proc->id);
        h.word(uint16_t(proc->name));
        h.word(a->actor_status);
        h.word(a->actor_condition);
        h.place(a->home);
        h.place(a->old);
        h.place(a->current);
        h.ang(a->shape_angle);
        h.vec(a->scale);
        h.vec(a->speed);
        h.f(a->speedF);
        h.f(a->gravity);
        h.f(a->maxFallSpeed);
        h.vec(a->eyePos);
        h.vec(a->attention_info.position);
        h.word(a->attention_info.flags);
        h.word(uint16_t(a->health));
    }
    return h.h;
}

void receipts() {
    twv::vita::telemetryEvent("visual_skip_draws", (long long)sStats.draws);
    twv::vita::telemetryEvent("visual_skip_calcs", (long long)sStats.calcs);
    twv::vita::telemetryEvent("visual_skip_replays", (long long)sStats.replays);
    twv::vita::telemetryEvent("visual_skip_full", (long long)sStats.full);
    twv::vita::telemetryEvent("visual_skip_rests", (long long)sStats.rests);
}
}  // namespace

namespace twv::vita {
VisualSkipStats visualSkipStats() {
    return sStats;
}
}

extern "C" int twv_vita_visual_skip_select(int which, int on) {
    if (on != 0 && on != 1) return 0;
    if (which == twv::vita::SkipHiddenDraw) twv_skip_hidden_draw_mode = on;
    else if (which == twv::vita::SkipOffscreenAnim) twv_skip_offscreen_anim_mode = on;
    else if (which == twv::vita::RestCreatures) twv_rest_creatures_mode = on;
    else return 0;
    return 1;
}

extern "C" int twv_vita_crowd_select(int level) {
    if (level < 0 || level >= twv::vita::CrowdLevelCount) return 0;
    twv_crowd_level = level;
    return 1;
}

extern "C" void twv_vita_state_digest_select(int on) {
    sDigest = on != 0;
}

extern "C" void twv_vita_visual_skip_count(int what) {
    switch (what) {
    case 0: ++sStats.draws; break;
    case 1: ++sStats.calcs; break;
    case 2: ++sStats.replays; break;
    case 3: ++sStats.full; break;
    case 4: ++sStats.rests; break;
    default: break;
    }
}

// m_Do_main.cpp's per-tick hook (patches/dusklight/0012). The fight benchmark's fixture.cpp defines
// it in a benchmark build (and calls twv_vita_visual_skip_tick); this weak one serves every other.
extern "C" __attribute__((weak)) void twv_vita_benchmark_tick() {
    twv_vita_visual_skip_tick();
}

extern "C" void twv_vita_visual_skip_tick(void) {
    const unsigned tick = ++twv_vita_tick_serial;
    if ((twv_skip_hidden_draw_mode || twv_skip_offscreen_anim_mode || twv_rest_creatures_mode) &&
        tick % ReceiptTicks == 0)
        receipts();
    if (!sDigest) return;
    unsigned count = 0;
    const uint32_t actors = actors_hash(count);
    Hash save;
    save.bytes(&g_dComIfG_gameInfo.info, sizeof g_dComIfG_gameInfo.info);
    int r[6];
    twv_cM_rnd_state(r);
    twv_cM_rnd2_state(r + 3);
    Hash rnd;
    for (int v : r) rnd.word(uint32_t(v));
    char text[96];
    std::snprintf(text, sizeof text, "t=%u;a=%08lx;s=%08lx;r=%08lx;n=%u", tick, (unsigned long)actors,
                  (unsigned long)save.h, (unsigned long)rnd.h, count);
    twv::vita::telemetryText("gs_digest", text);
}
