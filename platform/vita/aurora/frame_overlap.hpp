// --vita-frame-overlap (patches/aurora/0009, patches/dusklight/0019): the game thread ends a frame
// without waiting for the GX worker, runs the next frame's first game tick while the worker finishes,
// and settles the frame (waits, presents, begins the next) before anything that may change what the
// frame's commands still read.
//
// Without it, aurora_end_frame drains the FIFO (waits for the worker) and presents, and the next
// loop's aurora_begin_frame begins the next frame at once. With it:
//   end:    publishes the frame, waits only for its GXSetDrawDone (the draw-done callback then runs
//           before end returns, as with the drain), closes the frame for publish() and returns;
//   begin:  while a frame is pending, the begin is deferred to the settle (the next frame's commands
//           are not published, so the worker sees nothing of them before the present);
//   settle: wait for the frame's end, drop its bytes, the rest of end_frame (present, receipts),
//           then the deferred begin. Settle points: any FIFO drain (GXDrawDone), before a second
//           game tick and before the presentation draw (m_Do_main.cpp), a viewport policy change,
//           a frame that cannot be begun, the next end, and shutdown.
// The overlap is the original GameCube's: the GPU drew the frame the painter had just queued while
// the CPU ran the next fpcM_Management (deletes, creates, execute, draw), and the game itself waits
// for it at the end of that draw phase (mDoGph_AfterOfDraw -> JFWDisplay::endFrame ->
// JFWDrawDoneAlarm -> GXDrawDone, double XFB). That drain is where a tick normally settles; the
// second-tick and presentation points only cover a tick without its draw phase.
// Header-only, no SDK or game types: tests/frame_overlap_test.cpp drives it with recording ops.
#pragma once
#include <array>
#include <cstdint>

namespace twv::vita {
enum class SettleSite : unsigned {SecondTick=1, Presentation=2, FrameTick=3, Drain=4, Other=5};
constexpr unsigned SettleSites=6;
inline const char* settle_site_name(unsigned site) {
    constexpr const char* names[SettleSites]={"none","second_tick","presentation","frame_tick","drain","other"};
    return site<SettleSites?names[site]:"invalid";
}

// Ops (the real ones are in aurora_vita.cpp):
//   uint64_t publish_frame();   void wait_draw_done(uint64_t end);   void close_frame();
//   bool settle_frame(uint64_t end) (true: the worker was already done, no wait);
//   void finish_frame();        bool begin_now();
template<class Ops> struct FrameOverlap {
    Ops ops;
    bool enabled=false;
    bool pending=false;       // ended, not yet presented
    bool beginDeferred=false; // aurora_begin_frame was called while pending
    bool settling=false;
    uint64_t end=0;
    struct Stats {
        uint64_t frames=0;   // frames ended without waiting
        uint64_t idle=0;     // settles that found the worker done (no wait)
        std::array<uint64_t,SettleSites> sites{};
    } stats;

    // aurora_end_frame with the overlap on.
    void end_frame() {
        settle(SettleSite::Other); // a frame no settle point finished (no tick, UI loop)
        end=ops.publish_frame();
        ops.wait_draw_done(end);
        ops.close_frame();
        pending=true;
        ++stats.frames;
    }
    // aurora_begin_frame: false (and the pending frame finished) when the window cannot present.
    bool begin_frame(bool presentable) {
        if(!presentable) {
            settle(SettleSite::Other);
            return false;
        }
        if(pending) {
            beginDeferred=true;
            return true;
        }
        return ops.begin_now();
    }
    void settle(SettleSite site) {
        if(!pending || settling)return;
        settling=true;
        const bool idle=ops.settle_frame(end);
        ops.finish_frame();
        pending=false;
        if(beginDeferred) {
            beginDeferred=false;
            ops.begin_now();
        }
        settling=false;
        ++stats.sites[unsigned(site)];
        if(idle)++stats.idle;
    }
};
}
