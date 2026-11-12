// Painter profile: measurement only, built with the original game's int-enum ABI. Link wrappers
// observe the painter's calls (m_Do_graphic.cpp mDoGph_Painter: draw lists, particle groups, shadows,
// 2D lists) with the same arguments, order and results. On sampled frames the 2D
// list loop (dDlst_list_c::draw) is the wrapper's own copy of the original loop, timing each entry.
//
// Full receipts only (--vita-receipts=full). One painted frame in four (painter_profile_stats.hpp)
// times every section (the outermost wrapped call; "other" is the rest of the painter), the frame
// limiter's sleep inside the painter, each JPA resource's draw (by user index, with its emitter draws
// and alive particles) and each 2D list entry by draw class. Other frames pay a flag test per call.
// Receipts per 30 painted frames (paintprof_*); tools/vita_bench_report.py --painter reads them.
#include "d/d_drawlist.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "JSystem/JParticle/JPAResource.h"
#include "telemetry.hpp"
#include "painter_profile_stats.hpp"
#include <cstdio>
#include <cstring>
#include <typeinfo>

using namespace twv::vita::paintprof;

namespace {
constexpr unsigned WindowFrames = 30;
constexpr unsigned TopJpa = 16, Top2D = 16, TopHud = 48;

inline uint64_t now_us() {return twv::vita::processTimeUs();}

struct Window {
    uint32_t frames = 0, sampledFrames = 0;
    uint64_t painterUs = 0;
    uint64_t sectionUs[SectionCount] = {};
    uint32_t sectionCalls[SectionCount] = {};
    uint64_t sleepUs = 0;
    uint32_t sleepCalls = 0;
    // Aurora's FIFO hand-over to the GX worker: publish() (display list calls, draw done) and
    // finish_draw() (every GXEnd, which publishes inside), each waking the worker.
    uint64_t publishUs = 0, finishUs = 0;
    uint32_t publishCalls = 0, finishCalls = 0;
    // Display list calls (J3D materials and shapes by reference, others copied), their publish
    // included.
    uint64_t staticDlUs = 0, dlUs = 0;
    uint32_t staticDlCalls = 0, dlCalls = 0;
    uint64_t reads = 0;
    KeyTable<256> jpa;
    KeyTable<128> d2;
    KeyTable<256> hud; // patches/dusklight/0043, 0063, 0065: HUD parts and panes, J3D packet parts
};
Window gWin;
uint64_t gFrameNo = 0, gWindowNo = 0, gPaintStart = 0;
bool gOn = false;     // this painter call is sampled
unsigned gDepth = 0;  // wrapped sections open

// The game's two random generators (c_math.cpp). In the original a painted picture is a game tick.
// Here a picture may show two ticks, so a painter part that draws random numbers makes the game's
// random sequence depend on frame timing (found in twilight Kakariko, 12 Nov). The painter's whole
// run is checked on every frame, each section on sampled frames; counts are run totals.
extern "C" void twv_cM_rnd_state(int* out);
extern "C" void twv_cM_rnd2_state(int* out);
uint32_t gRndFrames = 0;               // painted frames whose painter drew random numbers
uint32_t gRndSection[SectionCount] = {};  // sampled frames in which the section did
int gRndAtBegin[6];
inline void rnd_state(int* s) {
    twv_cM_rnd_state(s);
    twv_cM_rnd2_state(s + 3);
}

// Who calls copied display lists (sampled frames, run totals): the callers' return addresses, to be
// placed in the ELF with addr2line. Receipt paintprof_dl_callers, the 12 busiest as address:calls.
struct DlCaller {
    const void* address;
    uint32_t calls;
};
DlCaller gDlCallers[64];
void dl_caller(const void* address) {
    unsigned h = unsigned(reinterpret_cast<uintptr_t>(address) >> 1) & 63;
    for (unsigned probe = 0; probe < 64; ++probe, h = (h + 1) & 63) {
        if (gDlCallers[h].address == address) {++gDlCallers[h].calls; return;}
        if (gDlCallers[h].address == nullptr) {gDlCallers[h] = {address, 1}; return;}
    }
}

void publish() {
    using twv::vita::telemetryEvent;
    using twv::vita::telemetryText;
    Window& w = gWin;
    telemetryEvent("paintprof_window", (long long)++gWindowNo);
    telemetryEvent("paintprof_frames", w.frames);
    telemetryEvent("paintprof_sampled_frames", w.sampledFrames);
    telemetryEvent("paintprof_painter_us", (long long)w.painterUs);
    char text[200];
    uint64_t inSections = 0;
    for (unsigned s = 0; s < SecOther; ++s) inSections += w.sectionUs[s];
    w.sectionUs[SecOther] = w.painterUs > inSections ? w.painterUs - inSections : 0;
    w.sectionCalls[SecOther] = w.sampledFrames;
    for (unsigned s = 0; s < SectionCount; ++s) {
        if (!w.sectionCalls[s]) continue;
        part_text(text, sizeof text, SectionNames[s], w.sectionUs[s], w.sectionCalls[s]);
        telemetryText("paintprof_part", text);
    }
    telemetryEvent("paintprof_sleep_us", (long long)w.sleepUs);
    telemetryEvent("paintprof_sleep_calls", w.sleepCalls);
    telemetryEvent("paintprof_publish_us", (long long)w.publishUs);
    telemetryEvent("paintprof_publish_calls", w.publishCalls);
    telemetryEvent("paintprof_finish_us", (long long)w.finishUs);
    telemetryEvent("paintprof_finish_calls", w.finishCalls);
    telemetryEvent("paintprof_static_dl_us", (long long)w.staticDlUs);
    telemetryEvent("paintprof_static_dl_calls", w.staticDlCalls);
    telemetryEvent("paintprof_dl_us", (long long)w.dlUs);
    telemetryEvent("paintprof_dl_calls", w.dlCalls);
    uint64_t us;
    uint32_t calls, items;
    w.jpa.total(us, calls, items);
    std::snprintf(text, sizeof text, "us=%llu;c=%lu;p=%lu", (unsigned long long)us, (unsigned long)calls,
                  (unsigned long)items);
    telemetryText("paintprof_jpa_total", text);
    unsigned top[TopJpa > Top2D ? TopJpa : Top2D];
    std::size_t used = w.jpa.top(top, TopJpa);
    for (std::size_t i = 0; i < used; ++i) {
        const Entry& e = w.jpa.slots[top[i]];
        jpa_text(text, sizeof text, unsigned(e.key), e);
        telemetryText("paintprof_jpa", text);
    }
    w.d2.total(us, calls, items);
    std::snprintf(text, sizeof text, "us=%llu;c=%lu", (unsigned long long)us, (unsigned long)calls);
    telemetryText("paintprof_2d_total", text);
    used = w.d2.top(top, Top2D);
    for (std::size_t i = 0; i < used; ++i) {
        const Entry& e = w.d2.slots[top[i]];
        class_text(text, sizeof text, e.name, e);
        telemetryText("paintprof_2d", text);
    }
    unsigned topHud[TopHud];
    used = w.hud.top(topHud, TopHud);
    for (std::size_t i = 0; i < used; ++i) {
        const Entry& e = w.hud.slots[topHud[i]];
        class_text(text, sizeof text, e.name, e);
        telemetryText("paintprof_hud", text);
    }
    {
        // The 12 busiest callers of copied display lists so far.
        unsigned order[12];
        unsigned n = 0;
        for (unsigned i = 0; i < 64; ++i) {
            if (!gDlCallers[i].calls) continue;
            unsigned pos = n < 12 ? n : 12;
            while (pos > 0 && gDlCallers[order[pos - 1]].calls < gDlCallers[i].calls) --pos;
            if (pos >= 12) continue;
            for (unsigned k = n < 12 ? n : 11; k > pos; --k) order[k] = order[k - 1];
            order[pos] = i;
            if (n < 12) ++n;
        }
        int at = 0;
        text[0] = 0;
        for (unsigned i = 0; i < n && at < 180; ++i)
            at += std::snprintf(text + at, sizeof text - at, "%s%lx:%lu", i ? " " : "",
                                (unsigned long)reinterpret_cast<uintptr_t>(gDlCallers[order[i]].address),
                                (unsigned long)gDlCallers[order[i]].calls);
        if (n) telemetryText("paintprof_dl_callers", text);
    }
    telemetryEvent("paintprof_rnd_frames", gRndFrames);
    if (gRndFrames) {
        int at = 0;
        text[0] = 0;
        for (unsigned s = 0; s < SectionCount && at < 170; ++s)
            if (gRndSection[s])
                at += std::snprintf(text + at, sizeof text - at, "%s%s:%lu", at ? " " : "", SectionNames[s],
                                    (unsigned long)gRndSection[s]);
        telemetryText("paintprof_rnd_parts", text);
    }
    telemetryEvent("paintprof_reads", (long long)w.reads);
    telemetryEvent("paintprof_period", Period);
    twv::vita::telemetryFlush();
    w.frames = w.sampledFrames = 0;
    w.painterUs = w.sleepUs = w.reads = w.publishUs = w.finishUs = w.staticDlUs = w.dlUs = 0;
    w.sleepCalls = w.publishCalls = w.finishCalls = w.staticDlCalls = w.dlCalls = 0;
    std::memset(w.sectionUs, 0, sizeof w.sectionUs);
    std::memset(w.sectionCalls, 0, sizeof w.sectionCalls);
    w.jpa.clear();
    w.d2.clear();
    w.hud.clear();
}

// The outermost wrapped call is charged to its section; nested wrapped calls stay in their caller's.
struct Scope {
    explicit Scope(unsigned s) : section(s), start(gDepth++ == 0 ? now_us() : 0) {
        if (gDepth == 1) rnd_state(rnd);
    }
    ~Scope() {
        if (--gDepth == 0) {
            gWin.sectionUs[section] += now_us() - start;
            ++gWin.sectionCalls[section];
            gWin.reads += 2;
            int now[6];
            rnd_state(now);
            if (std::memcmp(now, rnd, sizeof now) != 0) ++gRndSection[section];
        }
    }
    unsigned section;
    uint64_t start;
    int rnd[6];
};
}  // namespace

// patches/dusklight/0043 (d_meter2_draw.cpp, J2DPane.cpp): the HUD's parts and the panes under its
// meter screen, on sampled painter frames only (begin returns 0 otherwise).
extern "C" {
const void* twv_hud_root = nullptr;
}
extern "C" unsigned long long twv_paintprof_hud_begin(void) {
    if (!gOn) return 0;
    const uint64_t t = now_us();
    return t ? t : 1;
}
extern "C" void twv_paintprof_hud_end(const char* name, unsigned long long start) {
    if (!start || !gOn) return;
    Entry& e = gWin.hud.at(reinterpret_cast<uintptr_t>(name));
    if (!e.name) e.name = name;
    e.us += now_us() - start;
    ++e.calls;
    gWin.reads += 2;
}
// A pane's name: "d<depth>:" and its tag's characters, kept in a pool for the run (a HUD has a few
// dozen panes at these depths; past the pool's end they share one name).
extern "C" void twv_paintprof_hud_pane(const void* pane, uint64_t tag, int depth, unsigned long long start) {
    if (!start || !gOn) return;
    Entry& e = gWin.hud.at(reinterpret_cast<uintptr_t>(pane));
    if (!e.name) {
        // One name per pane for the whole run. (Until 12 Nov a name was taken from the pool again
        // in every window, so the pool ran out after a few windows and most panes were reported
        // under the shared name.)
        struct Named {
            const void* pane;
            char name[16];
        };
        static Named pool[256];
        const char* found = nullptr;
        unsigned h = unsigned((reinterpret_cast<uintptr_t>(pane) >> 3) ^ (reinterpret_cast<uintptr_t>(pane) >> 11)) & 255;
        for (unsigned probe = 0; probe < 16 && found == nullptr; ++probe, h = (h + 1) & 255) {
            Named& slot = pool[h];
            if (slot.pane == pane) {
                found = slot.name;
            } else if (slot.pane == nullptr) {
                slot.pane = pane;
                int k = std::snprintf(slot.name, sizeof slot.name, "d%d:", depth);
                for (int shift = 56; shift >= 0 && k < 15; shift -= 8) {
                    const char c = char((tag >> shift) & 0xFF);
                    if (c) slot.name[k++] = (c >= 32 && c < 127) ? c : '?';
                }
                slot.name[k] = 0;
                found = slot.name;
            }
        }
        e.name = found != nullptr ? found : "d?:more";
    }
    e.us += now_us() - start;
    ++e.calls;
    gWin.reads += 2;
}

// actor_profile.cpp (the painter phase of profile_hooks.cpp): the painter begins and ends.
extern "C" void twv_paintprof_begin(unsigned long long start) {
    if (twv::vita::telemetryIsQuiet()) {gOn = false; return;}
    gOn = sampled(++gFrameNo);
    gDepth = 0;
    gPaintStart = start;
    rnd_state(gRndAtBegin);
}
extern "C" void twv_paintprof_end(unsigned long long start, unsigned long long end) {
    if (twv::vita::telemetryIsQuiet()) {
        if (gWin.frames) std::memset(static_cast<void*>(&gWin), 0, sizeof gWin);
        gOn = false;
        return;
    }
    if (gOn) {
        ++gWin.sampledFrames;
        gWin.painterUs += end - start;
    }
    gOn = false;
    int rndNow[6];
    rnd_state(rndNow);
    if (std::memcmp(rndNow, gRndAtBegin, sizeof rndNow) != 0) ++gRndFrames;
    if (++gWin.frames >= WindowFrames) publish();
}

#define TWV_WRAP0(sym, sec)                                                         \
    extern "C" void __real_##sym();                                                 \
    extern "C" void __wrap_##sym() {                                                \
        if (!gOn) {__real_##sym(); return;}                                         \
        Scope scope(sec);                                                           \
        __real_##sym();                                                             \
    }
#define TWV_WRAP1(sym, sec)                                                         \
    extern "C" void __real_##sym(void*);                                            \
    extern "C" void __wrap_##sym(void* a) {                                         \
        if (!gOn) {__real_##sym(a); return;}                                        \
        Scope scope(sec);                                                           \
        __real_##sym(a);                                                            \
    }

TWV_WRAP0(_Z19dComIfGd_drawCopy2Dv, SecCopy2D)
TWV_WRAP1(_Z24dComIfGd_imageDrawShadowPA4_f, SecImageShadow)
TWV_WRAP0(_Z12dKy_setLightv, SecSetLight)
TWV_WRAP0(_Z23dComIfGd_drawOpaListSkyv, SecSkyOpa)
TWV_WRAP0(_Z23dComIfGd_drawXluListSkyv, SecSkyXlu)
TWV_WRAP0(_Z22dComIfGd_drawOpaListBGv, SecBgOpa)
TWV_WRAP0(_Z26dComIfGd_drawOpaListDarkBGv, SecBgOpaDark)
TWV_WRAP0(_Z26dComIfGd_drawOpaListMiddlev, SecMiddleOpa)
TWV_WRAP1(_Z31dComIfGp_particle_drawFogPri0_BP11JPADrawInfo, SecPaFogPri0B)
TWV_WRAP1(_Z34dComIfGp_particle_drawNormalPri0_BP11JPADrawInfo, SecPaNormalPri0B)
TWV_WRAP1(_Z19dComIfGd_drawShadowPA4_f, SecShadow)
TWV_WRAP0(_Z20dComIfGd_drawOpaListv, SecOpa)
TWV_WRAP0(_Z24dComIfGd_drawOpaListDarkv, SecOpaDark)
TWV_WRAP0(_Z26dComIfGd_drawOpaListPacketv, SecOpaPacket)
TWV_WRAP0(_Z22dComIfGd_drawXluListBGv, SecBgXlu)
TWV_WRAP0(_Z26dComIfGd_drawXluListDarkBGv, SecBgXluDark)
TWV_WRAP1(_Z31dComIfGp_particle_drawFogPri0_AP11JPADrawInfo, SecPaFogPri0A)
TWV_WRAP1(_Z34dComIfGp_particle_drawNormalPri0_AP11JPADrawInfo, SecPaNormalPri0A)
TWV_WRAP0(_Z20dComIfGd_drawXluListv, SecXlu)
TWV_WRAP0(_Z24dComIfGd_drawXluListDarkv, SecXluDark)
TWV_WRAP1(_ZN6J3DSys8drawInitEv, SecJ3dInit)
TWV_WRAP1(_ZN6J3DSys8reinitGXEv, SecJ3dInit)
TWV_WRAP0(_Z29dComIfGd_drawOpaListInvisiblev, SecOpaInvisible)
TWV_WRAP0(_Z29dComIfGd_drawXluListInvisiblev, SecXluInvisible)
TWV_WRAP1(_Z29dComIfGp_particle_drawFogPri4P11JPADrawInfo, SecPaFogPri4)
TWV_WRAP1(_Z32dComIfGp_particle_drawProjectionP11JPADrawInfo, SecPaProjection)
TWV_WRAP0(_Z21dComIfGd_drawListZxluv, SecZxlu)
TWV_WRAP0(_Z26dComIfGd_drawOpaListFilterv, SecFilter)
TWV_WRAP1(_Z29dComIfGp_particle_drawFogPri1P11JPADrawInfo, SecPaFogPri1)
TWV_WRAP1(_Z22dComIfGp_particle_drawP11JPADrawInfo, SecPaNormal)
TWV_WRAP1(_Z29dComIfGp_particle_drawFogPri2P11JPADrawInfo, SecPaFogPri2)
TWV_WRAP1(_Z25dComIfGp_particle_drawFogP11JPADrawInfo, SecPaFog)
TWV_WRAP1(_Z29dComIfGp_particle_drawFogPri3P11JPADrawInfo, SecPaFogPri3)
TWV_WRAP1(_Z24dComIfGp_particle_drawP1P11JPADrawInfo, SecPaP1)
TWV_WRAP1(_Z31dComIfGp_particle_drawDarkworldP11JPADrawInfo, SecPaDarkworld)
TWV_WRAP1(_Z28dComIfGp_particle_drawScreenP11JPADrawInfo, SecPaScreen)
TWV_WRAP0(_Z22dComIfGd_drawIndScreenv, SecIndScreen)
TWV_WRAP0(_Z28dComIfGd_drawXluList2DScreenv, Sec2DScreen)
TWV_WRAP0(_Z26dComIfGd_drawOpaList3Dlastv, Sec3Dlast)
TWV_WRAP1(_Z28dComIfGp_particle_draw2DgameP11JPADrawInfo, SecPa2Dgame)
TWV_WRAP0(_ZN12dDlst_list_c8calcWipeEv, SecCalcWipe)
TWV_WRAP1(_Z28dComIfGp_particle_draw2DbackP11JPADrawInfo, SecPa2Dback)
TWV_WRAP1(_Z32dComIfGp_particle_draw2DmenuBackP11JPADrawInfo, SecPa2DmenuBack)
TWV_WRAP0(_Z18dComIfGd_draw2DOpav, Sec2DOpa)
TWV_WRAP0(_Z21dComIfGd_draw2DOpaTopv, Sec2DOpaTop)
TWV_WRAP0(_Z18dComIfGd_draw2DXluv, Sec2DXlu)
TWV_WRAP1(_Z28dComIfGp_particle_draw2DforeP11JPADrawInfo, SecPa2Dfore)
TWV_WRAP1(_Z32dComIfGp_particle_draw2DmenuForeP11JPADrawInfo, SecPa2DmenuFore)

// The frame limiter's sleep inside the painter (JFWDisplay::beginRender, a virtual call, so inside
// "other"): profile_hooks.cpp times it on every frame and reports it here.
extern "C" void twv_paintprof_sleep(uint64_t us) {
    if (!gOn) return;
    gWin.sleepUs += us;
    ++gWin.sleepCalls;
    gWin.reads += 2;
}

// Aurora's FIFO hand-over inside the painter (GXCallDisplayList*, GXSetDrawDone: publish; GXEnd:
// finish_draw, whose own publish is not wrapped, so the two never nest).
extern "C" void __real__ZN6aurora2gx4fifo7publishEv();
extern "C" void __wrap__ZN6aurora2gx4fifo7publishEv() {
    if (!gOn) {__real__ZN6aurora2gx4fifo7publishEv(); return;}
    const uint64_t start = now_us();
    __real__ZN6aurora2gx4fifo7publishEv();
    gWin.publishUs += now_us() - start;
    ++gWin.publishCalls;
    gWin.reads += 2;
}
extern "C" void __real__ZN6aurora2gx4fifo11finish_drawEv();
extern "C" void __wrap__ZN6aurora2gx4fifo11finish_drawEv() {
    if (!gOn) {__real__ZN6aurora2gx4fifo11finish_drawEv(); return;}
    const uint64_t start = now_us();
    __real__ZN6aurora2gx4fifo11finish_drawEv();
    gWin.finishUs += now_us() - start;
    ++gWin.finishCalls;
    gWin.reads += 2;
}

// Display list calls inside the painter (Aurora GXDispList.cpp; their publish nests inside).
extern "C" void __real_GXCallDisplayListStatic(const void*, uint32_t);
extern "C" void __wrap_GXCallDisplayListStatic(const void* list, uint32_t size) {
    if (!gOn) {__real_GXCallDisplayListStatic(list, size); return;}
    const uint64_t start = now_us();
    __real_GXCallDisplayListStatic(list, size);
    gWin.staticDlUs += now_us() - start;
    ++gWin.staticDlCalls;
    gWin.reads += 2;
}
extern "C" void __real_GXCallDisplayList(const void*, uint32_t);
extern "C" void __wrap_GXCallDisplayList(const void* list, uint32_t size) {
    if (!gOn) {__real_GXCallDisplayList(list, size); return;}
    dl_caller(__builtin_return_address(0));
    const uint64_t start = now_us();
    __real_GXCallDisplayList(list, size);
    gWin.dlUs += now_us() - start;
    ++gWin.dlCalls;
    gWin.reads += 2;
}

// Each emitter's draw (JPAEmitterManager::draw), by its resource's user index.
extern "C" void __real__ZN11JPAResource4drawEP18JPAEmitterWorkDataP14JPABaseEmitter(JPAResource*, JPAEmitterWorkData*,
                                                                                  JPABaseEmitter*);
extern "C" void __wrap__ZN11JPAResource4drawEP18JPAEmitterWorkDataP14JPABaseEmitter(JPAResource* res,
                                                                                  JPAEmitterWorkData* work,
                                                                                  JPABaseEmitter* emtr) {
    if (!gOn) {__real__ZN11JPAResource4drawEP18JPAEmitterWorkDataP14JPABaseEmitter(res, work, emtr); return;}
    const uint32_t particles = emtr->getParticleNumber();
    const uint64_t start = now_us();
    __real__ZN11JPAResource4drawEP18JPAEmitterWorkDataP14JPABaseEmitter(res, work, emtr);
    Entry& e = gWin.jpa.at(res->getUsrIdx());
    e.us += now_us() - start;
    ++e.calls;
    e.items += particles;
    gWin.reads += 2;
}

// The 2D lists (copy, opaque, opaque top, translucent): on sampled frames the original loop, each
// entry timed by its draw class.
extern "C" void __real__ZN12dDlst_list_c4drawEPP12dDlst_base_cS2_(dDlst_list_c*, dDlst_base_c**, dDlst_base_c**);
extern "C" void __wrap__ZN12dDlst_list_c4drawEPP12dDlst_base_cS2_(dDlst_list_c* self, dDlst_base_c** p_start,
                                                                    dDlst_base_c** p_end) {
    if (!gOn) {__real__ZN12dDlst_list_c4drawEPP12dDlst_base_cS2_(self, p_start, p_end); return;}
    for (; p_start < p_end; p_start++) {
        dDlst_base_c* dlst = *p_start;
        const uint64_t start = now_us();
        dlst->draw();
        Entry& e = gWin.d2.at(*reinterpret_cast<const uintptr_t*>(dlst));
        if (!e.name) e.name = typeid(*dlst).name();
        e.us += now_us() - start;
        ++e.calls;
        gWin.reads += 2;
    }
}
