// Visual-only speed-ups and the game-state digest (visual_skips.hpp). Game thread only.
#include "runtime/visual_skips.hpp"
#include "runtime/telemetry.hpp"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_tag.h"
#include "f_op/f_op_camera_mng.h"
#include "d/actor/d_a_alink.h"
#include "d/d_kankyo.h"
#include "SSystem/SComponent/c_tag.h"
#include <psp2/kernel/threadmgr.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
int twv_skip_hidden_draw_mode = 0;
int twv_skip_offscreen_anim_mode = 0;
int twv_rest_creatures_mode = 0;
int twv_sync_loads_mode = 0;
int twv_draw_rnd_watch = 0;
int twv_fast_present_bg_mode = 0;
int twv_fast_particle_palette_mode = 0;
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
// patches/dusklight/0056: emitter draws and their particles by outcome.
unsigned long long sBatchEmitters[twv::vita::ParticleBatchReasons], sBatchParticles[twv::vita::ParticleBatchReasons];

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

// Test only (--vita-state-digest=on): 32 block hashes of an object's memory per game tick, to find
// state outside the game-state digest that differs between two runs (receipts gs_mem_link,
// gs_mem_env, gs_mem_cam). z is the object's size; each block is z/32 bytes, rounded up. Useful
// only while both runs allocate at the same addresses (pointers are hashed as they are).
void mem_digest(const char* name, unsigned tick, const void* p, std::size_t size) {
    if (p == nullptr || size == 0) return;
    char text[200];
    int at = std::snprintf(text, sizeof text, "t=%u;z=%u;h=", tick, unsigned(size));
    const std::size_t block = (size + 31) / 32;
    for (std::size_t off = 0; off < size && at < 190; off += block) {
        Hash h;
        h.bytes(static_cast<const char*>(p) + off, off + block <= size ? block : size - off);
        at += std::snprintf(text + at, sizeof text - at, "%04x", unsigned((h.h ^ (h.h >> 16)) & 0xFFFF));
    }
    twv::vita::telemetryText(name, text);
}

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

// --vita-sync-loads: only the thread that selected the mode (the game thread) waits; a request
// queued from another thread is left alone. A wait that lasts 15 s is given up and counted.
namespace {
SceUID sSyncThread = -1;
unsigned long long sSyncWaits = 0, sSyncSleeps = 0, sSyncTimeouts = 0;
}
extern "C" void twv_vita_fast_particle_palette_select(int on) {
    twv_fast_particle_palette_mode = on ? 1 : 0;
}
extern "C" void twv_vita_fast_present_bg_select(int on) {
    twv_fast_present_bg_mode = on ? 1 : 0;
}
extern "C" void twv_vita_sync_loads_select(int on) {
    twv_sync_loads_mode = on ? 1 : 0;
    sSyncThread = sceKernelGetThreadId();
}
extern "C" void twv_vita_sync_load_wait(const bool* done) {
    if (!done || sceKernelGetThreadId() != sSyncThread) return;
    const volatile bool* flag = done;
    ++sSyncWaits;
    for (unsigned i = 0; !*flag; ++i) {
        if (i == 30000) {
            ++sSyncTimeouts;
            twv::vita::telemetryEvent("sync_loads_timeouts", (long long)sSyncTimeouts);
            return;
        }
        sceKernelDelayThread(500);
        ++sSyncSleeps;
    }
}

// patches/dusklight/0062: draws that drew random numbers, per actor type.
namespace {
unsigned sDrawRnd[1024];
bool sDrawRndNew = false;
}
extern "C" void twv_vita_draw_rnd_note(int name) {
    if (name < 0 || name >= 1024) return;
    if (sDrawRnd[name]++ == 0) sDrawRndNew = true;
}

// patches/dusklight/0064 (test only): random numbers drawn by each actor type's execute in the
// current game tick.
extern "C" unsigned twv_rnd_calls;
namespace {
struct ExecRnd {
    int name;
    unsigned calls;
};
ExecRnd sExecRnd[24];
unsigned sExecRndUsed = 0, sExecRndMore = 0, sRndAtTickEnd = 0;
}
extern "C" void twv_vita_exec_rnd_note(int name, unsigned calls) {
    for (unsigned i = 0; i < sExecRndUsed; ++i)
        if (sExecRnd[i].name == name) {
            sExecRnd[i].calls += calls;
            return;
        }
    if (sExecRndUsed < 24) sExecRnd[sExecRndUsed++] = {name, calls};
    else sExecRndMore += calls;
}

// The callers that drew random numbers during Link's execute this tick (first generator; c_math.cpp
// keeps their return addresses). Written when their number differs from the tick before, with the
// address of cM_initRnd so that the addresses can be placed in the ELF.
extern "C" const void* twv_rnd_trace_base(void);
namespace {
unsigned sLinkRndLast = ~0u;
}
extern "C" void twv_vita_exec_rnd_trace(const void* const* callers, unsigned n) {
    if (n == sLinkRndLast) return;
    sLinkRndLast = n;
    char text[256];
    int at = std::snprintf(text, sizeof text, "t=%u;base=%lx;a=", twv_vita_tick_serial + 1,
                           (unsigned long)reinterpret_cast<uintptr_t>(twv_rnd_trace_base()));
    for (unsigned i = 0; i < n && at < 240; ++i)
        at += std::snprintf(text + at, sizeof text - at, "%s%lx", i ? "," : "",
                            (unsigned long)reinterpret_cast<uintptr_t>(callers[i]));
    twv::vita::telemetryText("alink_rnd", text);
}

extern "C" void twv_vita_state_digest_select(int on) {
    sDigest = on != 0;
    twv_draw_rnd_watch = on != 0;
}

extern "C" void twv_vita_particle_batch_note(int reason, int particles) {
    if (reason < 0 || reason >= twv::vita::ParticleBatchReasons || particles <= 0) return;
    ++sBatchEmitters[reason];
    sBatchParticles[reason] += (unsigned)particles;
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

// --vita-place (visual_skips.hpp).
namespace {
bool sPlacePending = false;
cXyz sPlacePos;
s16 sPlaceAngle = 0;
unsigned sPlaceSeen = 0; // ticks Link has existed
}
extern "C" void twv_vita_place_select(float x, float y, float z, int angle) {
    sPlacePos.set(x, y, z);
    sPlaceAngle = s16(angle);
    sPlacePending = true;
    sPlaceSeen = 0;
}

extern "C" void twv_vita_visual_skip_tick(void) {
    const unsigned tick = ++twv_vita_tick_serial;
    if (sPlacePending) {
        fopAc_ac_c* link = dComIfGp_getPlayer(0);
        if (link != nullptr && fopAcM_GetName(link) == fpcNm_ALINK_e && ++sPlaceSeen >= 30) {
            link->current.pos = sPlacePos;
            link->old.pos = sPlacePos;
            link->current.angle.y = sPlaceAngle;
            link->shape_angle.y = sPlaceAngle;
            link->speedF = 0.0f;
            link->speed.set(0.0f, 0.0f, 0.0f);
            sPlacePending = false;
            twv::vita::telemetryEvent("place_applied", (long long)tick);
        }
    }
    // With the test switch every ReceiptTicks; with only the hidden draw skip on, when a type is new.
    if (twv_draw_rnd_watch ? (sDrawRndNew || tick % ReceiptTicks == 0)
                           : (twv_skip_hidden_draw_mode != 0 && sDrawRndNew)) {
        char text[256];
        int at = 0;
        text[0] = 0;
        for (int name = 0; name < 1024 && at < 230; ++name)
            if (sDrawRnd[name]) at += std::snprintf(text + at, sizeof text - at, "%s%d:%u", at ? " " : "", name, sDrawRnd[name]);
        twv::vita::telemetryText("draw_rnd_types", text);
        sDrawRndNew = false;
    }
    if (twv_sync_loads_mode && tick % ReceiptTicks == 0) {
        twv::vita::telemetryEvent("sync_loads_waits", (long long)sSyncWaits);
        twv::vita::telemetryEvent("sync_loads_sleeps", (long long)sSyncSleeps);
    }
    if (tick % ReceiptTicks == 0) {
        char name[64];
        for (int reason = 0; reason < twv::vita::ParticleBatchReasons; ++reason) {
            std::snprintf(name, sizeof name, "particle_batch_%s_emitters", twv::vita::ParticleBatchReasonNames[reason]);
            twv::vita::telemetryEvent(name, (long long)sBatchEmitters[reason]);
            std::snprintf(name, sizeof name, "particle_batch_%s_particles", twv::vita::ParticleBatchReasonNames[reason]);
            twv::vita::telemetryEvent(name, (long long)sBatchParticles[reason]);
        }
    }
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
    if (fopAc_ac_c* link = dComIfGp_getPlayer(0); link != nullptr && fopAcM_GetName(link) == fpcNm_ALINK_e)
    {
        mem_digest("gs_mem_link", tick, link, sizeof(daAlink_c));
        // The part of Link's object where two Kakariko runs first differed (12 Nov), in 14-byte blocks.
        static const char* const fine[4] = {"gs_mem_link_2c00", "gs_mem_link_2dc0", "gs_mem_link_2f80", "gs_mem_link_3140"};
        for (unsigned i = 0; i < 4; ++i)
            if (0x2c00 + (i + 1) * 448u <= sizeof(daAlink_c))
                mem_digest(fine[i], tick, reinterpret_cast<const char*>(link) + 0x2c00 + i * 448u, 448);
    }
    mem_digest("gs_mem_env", tick, &g_env_light, sizeof g_env_light);
    if (camera_process_class* camera = dComIfGp_getCamera(0)) {
        mem_digest("gs_mem_cam", tick, camera, sizeof *camera);
        // The camera as this tick left it, one line per tick: a camera that jumps between two
        // places from one tick to the next shows here whatever the picture rate is.
        const auto& view = camera->view;
        char cam[176];
        std::snprintf(cam, sizeof cam, "t=%u eye=%.1f,%.1f,%.1f center=%.1f,%.1f,%.1f fov=%.2f", tick,
                      double(view.lookat.eye.x), double(view.lookat.eye.y), double(view.lookat.eye.z),
                      double(view.lookat.center.x), double(view.lookat.center.y), double(view.lookat.center.z),
                      double(view.fovy));
        twv::vita::telemetryText("gs_cam", cam);
    }
    // Who drew random numbers: n = numbers drawn since the end of the tick before (this tick and
    // whatever ran between the two), then "type:numbers" for the actor types' executes. Nested
    // executes count in both their own type and their caller's.
    const unsigned drawn = twv_rnd_calls - sRndAtTickEnd;
    sRndAtTickEnd = twv_rnd_calls;
    if (drawn != 0 || sExecRndUsed != 0) {
        char users[256];
        int at = std::snprintf(users, sizeof users, "t=%u;n=%u;e=", tick, drawn);
        for (unsigned i = 0; i < sExecRndUsed && at < 235; ++i)
            at += std::snprintf(users + at, sizeof users - at, "%s%d:%u", i ? "," : "", sExecRnd[i].name, sExecRnd[i].calls);
        if (sExecRndMore && at < 240) std::snprintf(users + at, sizeof users - at, ",more:%u", sExecRndMore);
        twv::vita::telemetryText("tick_rnd", users);
    }
    sExecRndUsed = sExecRndMore = 0;
}

// patches/dusklight/0070: an enemy's creation waited a tick because there is no player yet. The
// first sixteen waits are logged one by one (who, and for how many ticks so far); the total
// follows every sixteenth.
extern "C" void twv_enemy_without_player(int name, unsigned id, int waited_ticks) {
    static unsigned total = 0;
    ++total;
    if (total <= 16) {
        char text[80];
        std::snprintf(text, sizeof text, "name=%d id=%u waited=%d tick=%u", name, id, waited_ticks, twv_vita_tick_serial);
        twv::vita::telemetryText("enemy_waits_for_player", text);
    }
    if (total % 16 == 0) twv::vita::telemetryEvent("enemy_waits_for_player_total", (long long)total);
}
