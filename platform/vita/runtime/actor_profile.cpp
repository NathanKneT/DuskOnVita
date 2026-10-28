// Main-thread actor profile: measurement only, built with the original game's int-enum ABI.
// Link wrappers observe the original calls (same arguments, order and results); the one extra
// call, a second fopAcM_cullingCheck of an actor the first call culled (with the far plane moved
// out, to tell the far distance from the frustum), has no side effects and its result is unused.
//
// Full receipts only (--vita-receipts=full); free play pays flag tests. Frames end after the
// painter (profile_hooks.cpp phase hooks). One frame in eight times every process's execute and
// draw by profile name (actor frames), another times J3DModel's parts (J3D frames,
// patches/dusklight/0020); every frame records each tick's execute and draw handler time, split
// into a frame's final tick and the ticks whose draw lists the next tick resets. One window of
// receipts per 30 painted frames (actor_profile_stats.hpp holds the formats).
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_base.h"
#include "m_Do/m_Do_lib.h"
#include "d/d_stage.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "telemetry.hpp"
#include "actor_profile_stats.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace twv::vita::actprof;

extern "C" {
// Read by patches/dusklight/0020 (J3DModel.cpp) before each part.
unsigned char twv_j3d_prof_on = 0;
// Read by patches/dusklight/0030 (execute detail scopes) and the wraps below: 1 while a sub frame's
// execute handler runs.
unsigned char twv_actsub_on = 0;
// runtime/fast_actors.cpp: the fast paths' totals with each receipt window.
void twv_vita_fast_actor_publish(void) __attribute__((weak));
// painter_profile.cpp: the painter's sections, on its own sampled frames (full receipts only).
void twv_paintprof_begin(unsigned long long start);
void twv_paintprof_end(unsigned long long start, unsigned long long end);
// fast_painter.cpp: its receipts, after every painter (quiet receipts too).
void twv_vita_fast_painter_frame(void);
}

namespace {
constexpr unsigned WindowFrames = 30;
constexpr unsigned MaxTicks = 4;
constexpr unsigned TopTypes = 12;
constexpr unsigned MaxDepth = 8;
// profile_hooks.cpp phase numbers.
constexpr unsigned PhaseExecute = 0, PhaseDraw = 1, PhasePainter = 2;

inline uint64_t now_us() {return twv::vita::processTimeUs();}

struct Window {
    TypeStats types[TypeSlots];
    uint32_t frames = 0, framesByTicks[3] = {};
    // Every frame; [0] the frame's final tick, [1] earlier ticks of the same frame.
    uint64_t tickExecUs[2] = {}, tickDrawUs[2] = {};
    uint32_t ticks[2] = {};
    uint64_t presentUs = 0;
    // Actor frames.
    uint32_t actorFrames = 0, actorTicks = 0;
    uint64_t actorExecUs = 0, actorDrawUs = 0, nonActorDrawSelfUs = 0, handlerExecUs = 0, handlerDrawUs = 0;
    uint32_t drew = 0, culledFrustum = 0, culledFar = 0, skipped = 0;
    uint32_t distCount[DistanceBins] = {};
    uint64_t distUs[DistanceBins] = {};
    // J3D frames.
    uint32_t j3dFrames = 0, j3dTicks = 0;
    PartStats parts[WhereCount];
    uint64_t j3dExecUs = 0, j3dDrawUs[2] = {};
    uint32_t j3dDrawTicks[2] = {};
    // Painter on sampled frames (both kinds): J3DDrawBuffer::draw inside it.
    uint32_t painterFrames = 0, painterJ3dCalls = 0;
    uint64_t painterUs = 0, painterJ3dUs = 0;
    uint64_t reads = 0;
    // Sub frames: execute detail per profile (actor_profile_stats.hpp Sub).
    uint32_t subFrames = 0, subTicks = 0;
    uint64_t subExecUs = 0;
    uint32_t subUs[TypeSlots][SubCount];
    uint32_t subCalls[TypeSlots][SubCount];
    uint32_t subExecSlotUs[TypeSlots];
};
Window gWin;

struct FrameTicks {
    unsigned n = 0;
    uint64_t execUs[MaxTicks] = {}, drawUs[MaxTicks] = {};
    PartStats draw[MaxTicks];
};
FrameTicks gFrame;

Mode gMode = ModeNone;
uint64_t gFrameNo = 0, gWindowNo = 0;
Where gWhere = WhereOther;
bool gInDraw = false;

struct ExecChain {bool open = false; unsigned slot = 0; uint64_t start = 0, first = 0;};
ExecChain gExec;
unsigned gExecDepth = 0;

struct DrawFrame {uint64_t start, childUs; unsigned slot; int cull;};
DrawFrame gStack[MaxDepth];
unsigned gDepth = 0;

inline unsigned tick_index() {return gFrame.n ? (gFrame.n - 1 < MaxTicks ? gFrame.n - 1 : MaxTicks - 1) : 0;}

float camera_distance(const fopAc_ac_c* actor) {
    MtxP m = j3dSys.getViewMtx();
    const cXyz& p = actor->current.pos;
    const float x = m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3];
    const float y = m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3];
    const float z = m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3];
    return std::sqrt(x * x + y * y + z * z);
}

long long clock_ns() {
    static long long ns = -1;
    if (ns < 0) {
        constexpr int Reads = 256;
        const uint64_t start = now_us();
        for (int i = 0; i < Reads; ++i) (void)now_us();
        ns = (long long)((now_us() - start) * 1000 / (Reads + 1));
    }
    return ns;
}

void publish() {
    using twv::vita::telemetryEvent;
    using twv::vita::telemetryText;
    Window& w = gWin;
    telemetryEvent("actprof_window", (long long)++gWindowNo);
    telemetryEvent("actprof_frames", w.frames);
    const char* tickNames[3] = {"actprof_frames_0tick", "actprof_frames_1tick", "actprof_frames_2tick"};
    for (unsigned i = 0; i < 3; ++i) telemetryEvent(tickNames[i], w.framesByTicks[i]);
    telemetryEvent("actprof_ticks_final", w.ticks[0]);
    telemetryEvent("actprof_ticks_nonfinal", w.ticks[1]);
    telemetryEvent("actprof_tick_final_exec_us", (long long)w.tickExecUs[0]);
    telemetryEvent("actprof_tick_final_draw_us", (long long)w.tickDrawUs[0]);
    telemetryEvent("actprof_tick_nonfinal_exec_us", (long long)w.tickExecUs[1]);
    telemetryEvent("actprof_tick_nonfinal_draw_us", (long long)w.tickDrawUs[1]);
    telemetryEvent("actprof_present_us", (long long)w.presentUs);
    telemetryEvent("actprof_actor_frames", w.actorFrames);
    telemetryEvent("actprof_actor_ticks", w.actorTicks);
    telemetryEvent("actprof_actor_exec_us", (long long)w.actorExecUs);
    telemetryEvent("actprof_actor_draw_us", (long long)w.actorDrawUs);
    telemetryEvent("actprof_nonactor_draw_self_us", (long long)w.nonActorDrawSelfUs);
    telemetryEvent("actprof_handler_exec_us", (long long)w.handlerExecUs);
    telemetryEvent("actprof_handler_draw_us", (long long)w.handlerDrawUs);
    telemetryEvent("actprof_drew", w.drew);
    telemetryEvent("actprof_culled_frustum", w.culledFrustum);
    telemetryEvent("actprof_culled_far", w.culledFar);
    telemetryEvent("actprof_skipped", w.skipped);
    char text[240];
    std::snprintf(text, sizeof text, "n=%lu,%lu,%lu,%lu;us=%llu,%llu,%llu,%llu",
        (unsigned long)w.distCount[0], (unsigned long)w.distCount[1], (unsigned long)w.distCount[2],
        (unsigned long)w.distCount[3], (unsigned long long)w.distUs[0], (unsigned long long)w.distUs[1],
        (unsigned long long)w.distUs[2], (unsigned long long)w.distUs[3]);
    telemetryText("actprof_dist", text);
    unsigned top[TopTypes];
    const std::size_t used = top_types(w.types, TypeSlots, top, TopTypes);
    for (std::size_t i = 0; i < used; ++i) {
        const TypeStats& t = w.types[top[i]];
        char name[32];
        if (t.actor) {
            const char* s = dStage_getName(s16(top[i]), s8(t.argument));
            std::snprintf(name, sizeof name, "%s", s ? s : "?");
        } else {
            std::snprintf(name, sizeof name, "#%u", top[i]);
        }
        for (char* c = name; *c; ++c) if (*c == ';' || *c == '=') *c = '_';
        type_text(text, sizeof text, name, top[i], t);
        telemetryText("actprof_type", text);
    }
    telemetryEvent("actprof_j3d_frames", w.j3dFrames);
    telemetryEvent("actprof_j3d_ticks", w.j3dTicks);
    telemetryEvent("actprof_j3d_exec_us", (long long)w.j3dExecUs);
    telemetryEvent("actprof_j3d_draw_final_us", (long long)w.j3dDrawUs[0]);
    telemetryEvent("actprof_j3d_draw_final_ticks", w.j3dDrawTicks[0]);
    telemetryEvent("actprof_j3d_draw_nonfinal_us", (long long)w.j3dDrawUs[1]);
    telemetryEvent("actprof_j3d_draw_nonfinal_ticks", w.j3dDrawTicks[1]);
    for (unsigned k = 0; k < WhereCount; ++k) {
        part_text(text, sizeof text, WhereNames[k], w.parts[k]);
        telemetryText("actprof_j3d", text);
    }
    // Execute detail: the profiles with the most execute time on sub frames, then the no-process slot.
    telemetryEvent("actprof_sub_frames", w.subFrames);
    telemetryEvent("actprof_sub_ticks", w.subTicks);
    telemetryEvent("actprof_sub_exec_us", (long long)w.subExecUs);
    {
        constexpr unsigned SubTop = 16;
        unsigned subTop[SubTop + 1];
        unsigned used = 0;
        for (unsigned i = 0; i + 1 < TypeSlots; ++i) {
            if (!w.subExecSlotUs[i]) continue;
            unsigned pos = used < SubTop ? used : SubTop;
            while (pos > 0 && w.subExecSlotUs[subTop[pos - 1]] < w.subExecSlotUs[i]) --pos;
            if (pos >= SubTop) continue;
            for (unsigned k = (used < SubTop ? used : SubTop - 1); k > pos; --k) subTop[k] = subTop[k - 1];
            subTop[pos] = i;
            if (used < SubTop) ++used;
        }
        subTop[used++] = TypeSlots - 1;
        for (unsigned i = 0; i < used; ++i) {
            const unsigned slot = subTop[i];
            char head[48];
            std::snprintf(head, sizeof head, "p=%u;ex=%lu", slot, (unsigned long)w.subExecSlotUs[slot]);
            telemetryText("actprof_sub_type", head);
            unsigned next = 0;
            while (sub_text(text, sizeof text, slot, w.subUs[slot], w.subCalls[slot], &next))
                telemetryText("actprof_sub", text);
        }
    }
    telemetryEvent("actprof_painter_frames", w.painterFrames);
    telemetryEvent("actprof_painter_us", (long long)w.painterUs);
    telemetryEvent("actprof_painter_j3d_us", (long long)w.painterJ3dUs);
    telemetryEvent("actprof_painter_j3d_calls", w.painterJ3dCalls);
    telemetryEvent("actprof_reads", (long long)w.reads);
    telemetryEvent("actprof_clock_ns", clock_ns());
    telemetryEvent("actprof_period", Period);
    if (twv_vita_fast_actor_publish) twv_vita_fast_actor_publish();
    twv::vita::telemetryFlush();
    std::memset(static_cast<void*>(&w), 0, sizeof w);
}

// After the painter: the frame's ticks into the window, then the next frame's mode.
void frame_end() {
    Window& w = gWin;
    const unsigned n = gFrame.n < MaxTicks ? gFrame.n : MaxTicks;
    ++w.frames;
    ++w.framesByTicks[gFrame.n < 2 ? gFrame.n : 2];
    for (unsigned i = 0; i < n; ++i) {
        const unsigned k = i + 1 == n ? 0 : 1;
        w.tickExecUs[k] += gFrame.execUs[i];
        w.tickDrawUs[k] += gFrame.drawUs[i];
        ++w.ticks[k];
        if (gMode == ModeActors) {
            w.handlerExecUs += gFrame.execUs[i];
            w.handlerDrawUs += gFrame.drawUs[i];
        } else if (gMode == ModeJ3D) {
            w.j3dDrawUs[k] += gFrame.drawUs[i];
            ++w.j3dDrawTicks[k];
            w.j3dExecUs += gFrame.execUs[i];
            w.parts[k ? WhereDrawNonfinal : WhereDrawFinal].add(gFrame.draw[i]);
        }
    }
    if (gMode == ModeActors) {++w.actorFrames; w.actorTicks += n;}
    if (gMode == ModeJ3D) {++w.j3dFrames; w.j3dTicks += n;}
    if (gMode == ModeSub) {++w.subFrames; w.subTicks += n;}
    gFrame = FrameTicks{};
    if (w.frames >= WindowFrames) publish();
    gMode = twv::vita::telemetryIsQuiet() ? ModeNone : frame_mode(++gFrameNo);
    twv_j3d_prof_on = gMode == ModeJ3D ? 1 : 0;
    twv_actsub_on = 0;
    gWhere = WhereOther;
}

// Sub frames: the part's nesting depth (a part inside itself counts once).
unsigned gSubDepth[SubCount];
}  // namespace

// patches/dusklight/0030 scopes and the wraps below, while twv_actsub_on: begin returns the clock
// (never 0), end charges the part to the executing process's profile.
extern "C" unsigned long long twv_actsub_begin(unsigned sub) {
    if (sub >= SubCount) return 0;
    ++gSubDepth[sub];
    const uint64_t t = now_us();
    return t ? t : 1;
}
extern "C" void twv_actsub_end(unsigned sub, unsigned long long start) {
    const uint64_t t = now_us();
    if (sub >= SubCount || !gSubDepth[sub]) return;
    const unsigned slot = gExec.open ? gExec.slot : TypeSlots - 1;
    ++gWin.subCalls[slot][sub];
    if (--gSubDepth[sub] == 0) gWin.subUs[slot][sub] += uint32_t(t - start);
}
namespace {
struct SubScope {
    explicit SubScope(unsigned s) : sub(s), start(twv_actsub_on ? twv_actsub_begin(s) : 0) {}
    ~SubScope() {if (start) twv_actsub_end(sub, start);}
    SubScope(const SubScope&) = delete;
    SubScope& operator=(const SubScope&) = delete;
    unsigned sub;
    unsigned long long start;
};
}  // namespace

// profile_hooks.cpp: the tick's execute and draw handlers and the painter, with the times its
// wrappers read (end 0: the phase begins).
extern "C" void twv_actprof_phase(unsigned phase, unsigned long long start, unsigned long long end) {
    if (phase == PhasePainter && end) twv_vita_fast_painter_frame();
    if (twv::vita::telemetryIsQuiet()) {
        if (gMode != ModeNone || gWin.frames) {
            gMode = ModeNone;
            twv_j3d_prof_on = 0;
            std::memset(static_cast<void*>(&gWin), 0, sizeof gWin);
            gFrame = FrameTicks{};
        }
        return;
    }
    if (phase == PhaseExecute) {
        if (!end) {
            ++gFrame.n;
            gWhere = WhereExecute;
            gExec = ExecChain{};
            twv_actsub_on = gMode == ModeSub ? 1 : 0;
            return;
        }
        gFrame.execUs[tick_index()] += end - start;
        if (gExec.open && gMode == ModeSub) {
            gWin.subExecSlotUs[gExec.slot] += uint32_t(end - gExec.start);
            gWin.subExecUs += end - gExec.first;
        } else if (gExec.open) {
            gWin.types[gExec.slot].execUs += uint32_t(end - gExec.start);
            gWin.actorExecUs += end - gExec.first;
        }
        twv_actsub_on = 0;
        gExec = ExecChain{};
        gWhere = WhereOther;
    } else if (phase == PhaseDraw) {
        if (!end) {gInDraw = true; gWhere = WhereDrawFinal; gDepth = 0; return;}
        gFrame.drawUs[tick_index()] += end - start;
        gInDraw = false;
        gWhere = WhereOther;
    } else if (phase == PhasePainter) {
        if (!end) {gWhere = WherePainter; twv_paintprof_begin(start); return;}
        twv_paintprof_end(start, end);
        if (gMode != ModeNone) {++gWin.painterFrames; gWin.painterUs += end - start;}
        frame_end();
    }
}

// patches/dusklight/0020: J3DModel parts on J3D frames.
extern "C" unsigned long long twv_j3d_prof_begin(void) {return now_us();}
extern "C" void twv_j3d_prof_end(unsigned part, unsigned long long start) {
    const uint64_t us = now_us() - start;
    if (part >= PartCount) return;
    gWin.reads += 2;
    PartStats& s = gInDraw ? gFrame.draw[tick_index()] : gWin.parts[gWhere];
    s.us[part] += us;
    ++s.calls[part];
}

// Every process's execute, in line-queue order: one clock read per process, the segment until the
// next process (or the handler's end) charged to it. Nested calls stay in their caller's segment.
extern "C" int __real__Z13fpcEx_ExecuteP18base_process_class(base_process_class*);
extern "C" int __wrap__Z13fpcEx_ExecuteP18base_process_class(base_process_class* proc) {
    if ((gMode != ModeActors && gMode != ModeSub) || gWhere != WhereExecute || gExecDepth)
        return __real__Z13fpcEx_ExecuteP18base_process_class(proc);
    const uint64_t t = now_us();
    if (gMode == ModeSub) {
        // Execute detail: the same segments, charged to the sub frame's own totals.
        if (gExec.open) gWin.subExecSlotUs[gExec.slot] += uint32_t(t - gExec.start);
        else gExec.first = t;
        gExec.open = true; gExec.slot = type_slot(proc->profname); gExec.start = t;
        ++gExecDepth;
        const int ret = __real__Z13fpcEx_ExecuteP18base_process_class(proc);
        --gExecDepth;
        return ret;
    }
    ++gWin.reads;
    if (gExec.open) gWin.types[gExec.slot].execUs += uint32_t(t - gExec.start);
    else gExec.first = t;
    const unsigned slot = type_slot(proc->profname);
    TypeStats& type = gWin.types[slot];
    if (proc->state.init_state == 2) {
        ++type.execCalls;
        type.actor = fopAc_IsActor(proc) != 0;
    }
    gExec.open = true; gExec.slot = slot; gExec.start = t;
    ++gExecDepth;
    const int ret = __real__Z13fpcEx_ExecuteP18base_process_class(proc);
    --gExecDepth;
    return ret;
}

// Every process's draw in the tick's draw handler (the scene's draw calls its actors' draws):
// self time per profile, culling and camera distance for actors.
extern "C" int __real__Z13fpcDw_ExecuteP18base_process_class(base_process_class*);
extern "C" int __wrap__Z13fpcDw_ExecuteP18base_process_class(base_process_class* proc) {
    if (gMode != ModeActors || !gInDraw || gDepth >= MaxDepth)
        return __real__Z13fpcDw_ExecuteP18base_process_class(proc);
    DrawFrame& f = gStack[gDepth++];
    f.slot = type_slot(proc->profname);
    f.childUs = 0;
    f.cull = 0;
    f.start = now_us();
    const int ret = __real__Z13fpcDw_ExecuteP18base_process_class(proc);
    const uint64_t end = now_us();
    gWin.reads += 2;
    --gDepth;
    const uint64_t incl = end - f.start;
    const uint64_t self = incl > f.childUs ? incl - f.childUs : 0;
    if (gDepth) gStack[gDepth - 1].childUs += incl;
    else gWin.actorDrawUs += incl;
    TypeStats& t = gWin.types[f.slot];
    t.drawUs += uint32_t(self);
    if (fopAc_IsActor(proc)) {
        auto* actor = reinterpret_cast<fopAc_ac_c*>(proc);
        t.actor = true;
        t.argument = actor->argument;
        if (f.cull == 1) {++t.culledFrustum; ++gWin.culledFrustum;}
        else if (f.cull == 2) {++t.culledFar; ++gWin.culledFar;}
        else if (!fopAcM_CheckCondition(actor, fopAcCnd_NODRAW_e)) {
            ++t.drew; ++gWin.drew;
            const unsigned bin = distance_bin(camera_distance(actor));
            ++gWin.distCount[bin];
            gWin.distUs[bin] += self;
        } else {
            ++t.skipped; ++gWin.skipped;
        }
    } else {
        gWin.nonActorDrawSelfUs += self;
    }
    return ret;
}

// fopAc_Draw's culling test. A culled actor is tested again with the far plane out of reach:
// visible then means the far distance culled it, else the frustum (sides or near plane).
extern "C" s32 __real__Z19fopAcM_cullingCheckPK10fopAc_ac_c(const fopAc_ac_c*);
extern "C" s32 __wrap__Z19fopAcM_cullingCheckPK10fopAc_ac_c(const fopAc_ac_c* actor) {
    const s32 culled = __real__Z19fopAcM_cullingCheckPK10fopAc_ac_c(actor);
    if (!culled || gMode != ModeActors || !gInDraw || !gDepth) return culled;
    J3DUClipper& clipper = mDoLib_clipper::mClipper;
    const f32 savedFar = clipper.getFar(), savedSystemFar = mDoLib_clipper::mSystemFar;
    mDoLib_clipper::mSystemFar = 1.0e30f;
    clipper.setFar(1.0e30f);
    const s32 again = __real__Z19fopAcM_cullingCheckPK10fopAc_ac_c(actor);
    mDoLib_clipper::mSystemFar = savedSystemFar;
    clipper.setFar(savedFar);
    gStack[gDepth - 1].cull = again ? 1 : 2;
    return culled;
}

// The presentation pass (m_Do_main.cpp: fpcM_DrawIterater after the ticks; the draw handler's own
// call is inside f_pc_manager.cpp and is not wrapped).
extern "C" int __real__Z17fpcM_DrawIteraterPFiPvS_E(void*);
extern "C" int __wrap__Z17fpcM_DrawIteraterPFiPvS_E(void* func) {
    if (twv::vita::telemetryIsQuiet()) return __real__Z17fpcM_DrawIteraterPFiPvS_E(func);
    const uint64_t start = now_us();
    const Where saved = gWhere;
    gWhere = WherePresent;
    const int ret = __real__Z17fpcM_DrawIteraterPFiPvS_E(func);
    gWhere = saved;
    gWin.presentUs += now_us() - start;
    return ret;
}

// Execute detail (sub frames): link wrappers around calls that cross translation units. Same
// arguments, order and results; outside sub frames each pays one flag test. Pointer arguments as
// void* (same AAPCS registers and return values as the original declarations).
#define TWV_SUB_WRAP_VOID(sym, sub, params, args)                                                  \
    extern "C" void __real_##sym params;                                                           \
    extern "C" void __wrap_##sym params {                                                          \
        SubScope scope(sub);                                                                       \
        __real_##sym args;                                                                         \
    }
#define TWV_SUB_WRAP_RET(type, sym, sub, params, args)                                             \
    extern "C" type __real_##sym params;                                                           \
    extern "C" type __wrap_##sym params {                                                          \
        SubScope scope(sub);                                                                       \
        return __real_##sym args;                                                                  \
    }
TWV_SUB_WRAP_VOID(_ZN8J3DModel4calcEv, SubJ3dCalc, (void* self), (self))
TWV_SUB_WRAP_VOID(_ZN12J3DJointTree4calcEP12J3DMtxBufferRK3VecRA3_A4_Kf, SubJointTree,
                  (void* self, void* buffer, const void* scale, const void* mtx), (self, buffer, scale, mtx))
TWV_SUB_WRAP_VOID(_ZN12J3DMtxBuffer21calcWeightEnvelopeMtxEv, SubWeightEnv, (void* self), (self))
TWV_SUB_WRAP_RET(bool, _ZN4dBgS6SphChkEP11dBgS_SphChkPv, SubSph, (void* self, void* chk, void* user),
                 (self, chk, user))
TWV_SUB_WRAP_VOID(_ZN4dBgS11WallCorrectEP9dBgS_Acch, SubWall, (void* self, void* acch), (self, acch))
TWV_SUB_WRAP_VOID(_ZN4dBgS15WallCorrectSortEP9dBgS_Acch, SubWall, (void* self, void* acch), (self, acch))
TWV_SUB_WRAP_RET(float, _ZN4dBgS7RoofChkEP12dBgS_RoofChk, SubRoof, (void* self, void* chk), (self, chk))
TWV_SUB_WRAP_RET(void*, _Z16fpcLyIt_AllJudgePFPvS_S_ES_, SubSearchAll, (void* func, void* data), (func, data))
TWV_SUB_WRAP_RET(void*, _Z13fopAcIt_JudgePFPvS_S_ES_, SubSearchActor, (void* func, void* data), (func, data))
TWV_SUB_WRAP_RET(int, _ZN16mDoExt_McaMorfSO4playEja, SubMorfPlay, (void* self, unsigned mode, signed char rev),
                 (self, mode, rev))
TWV_SUB_WRAP_RET(int, _ZN14mDoExt_McaMorf4playEP3Vecja, SubMorfPlay,
                 (void* self, void* pos, unsigned mode, signed char rev), (self, pos, mode, rev))
TWV_SUB_WRAP_RET(int, _ZN15mDoExt_McaMorf24playEja, SubMorfPlay, (void* self, unsigned mode, signed char rev),
                 (self, mode, rev))
TWV_SUB_WRAP_VOID(_ZN10Z2Creature9frameworkEja, SubCreature, (void* self, unsigned mode, signed char rev),
                  (self, mode, rev))
TWV_SUB_WRAP_VOID(_ZN4cCcS3SetEP8cCcD_Obj, SubCcSet, (void* self, void* obj), (self, obj))
TWV_SUB_WRAP_VOID(_ZN10dMsgFlow_c4initEP10fopAc_ac_ciiPS1_, SubMsgFlow,
                  (void* self, void* partner, int flow, int param, void* partners), (self, partner, flow, param, partners))
TWV_SUB_WRAP_RET(bool, _ZN4dusk4flow13bind_resourceEPKvt, SubFlowBind, (const void* bmg, unsigned short group),
                 (bmg, group))
#undef TWV_SUB_WRAP_VOID
#undef TWV_SUB_WRAP_RET

// The painter's J3D part: each draw buffer's packets (GX command writing included), sampled frames.
extern "C" void __real__ZNK13J3DDrawBuffer4drawEv(const void*);
extern "C" void __wrap__ZNK13J3DDrawBuffer4drawEv(const void* self) {
    if (gMode == ModeNone || gWhere != WherePainter) {__real__ZNK13J3DDrawBuffer4drawEv(self); return;}
    const uint64_t start = now_us();
    __real__ZNK13J3DDrawBuffer4drawEv(self);
    gWin.painterJ3dUs += now_us() - start;
    ++gWin.painterJ3dCalls;
    gWin.reads += 2;
}
