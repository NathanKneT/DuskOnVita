// --vita-frame-overlap: the state machine (platform/vita/aurora/frame_overlap.hpp) with recording
// operations, and the FIFO functions of patches/aurora/0009, extracted from the actual patched
// lib/gx/fifo.cpp by tools/host_tests.sh, against a simulated worker.
#include "aurora/frame_overlap.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// --- the state machine ---
namespace machine {
std::vector<std::string> log;
uint64_t published = 0;
bool workerDone = false;
struct Ops {
    uint64_t publish_frame() {log.push_back("publish");return ++published;}
    void wait_draw_done(uint64_t end) {assert(end==published);log.push_back("wait_draw_done");}
    void close_frame() {log.push_back("close");}
    bool settle_frame(uint64_t end) {log.push_back("settle "+std::to_string(end));return workerDone;}
    void finish_frame() {log.push_back("finish");}
    bool begin_now() {log.push_back("begin");return true;}
};
using Log=std::vector<std::string>;
void test() {
    using twv::vita::SettleSite;
    twv::vita::FrameOverlap<Ops> o;
    o.enabled=true;
    // First frame: nothing pending, begin at once.
    assert(o.begin_frame(true) && log==(Log{"begin"}));
    log.clear();
    // End: published, draw-done waited for, closed; no settle, no present.
    o.end_frame();
    assert(log==(Log{"publish","wait_draw_done","close"}) && o.pending && o.end==1 && o.stats.frames==1);
    log.clear();
    // The next begin is deferred while the frame is pending.
    assert(o.begin_frame(true) && log.empty() && o.beginDeferred);
    // First tick runs here. The settle before the presentation finishes the frame, then begins.
    o.settle(SettleSite::Presentation);
    assert(log==(Log{"settle 1","finish","begin"}) && !o.pending && !o.beginDeferred);
    assert(o.stats.sites[2]==1 && o.stats.idle==0);
    log.clear();
    // Settling again (drain, second tick) does nothing.
    o.settle(SettleSite::Drain);
    o.settle(SettleSite::SecondTick);
    assert(log.empty() && o.stats.sites[4]==0 && o.stats.sites[1]==0);
    // Second frame; a second tick settles it; the worker was already done.
    o.end_frame();
    log.clear();
    assert(o.begin_frame(true));
    workerDone=true;
    o.settle(SettleSite::SecondTick);
    assert(log==(Log{"settle 2","finish","begin"}) && o.stats.sites[1]==1 && o.stats.idle==1);
    workerDone=false;
    log.clear();
    // An end with a frame still pending (no settle point reached: UI loop) settles first.
    o.end_frame();
    assert(o.begin_frame(true));
    log.clear();
    o.end_frame();
    assert(log==(Log{"settle 3","finish","begin","publish","wait_draw_done","close"}) && o.end==4);
    assert(o.stats.sites[5]==1);
    log.clear();
    // A window that cannot present finishes the pending frame and begins nothing.
    assert(!o.begin_frame(false) && log==(Log{"settle 4","finish"}) && !o.pending && !o.beginDeferred);
    log.clear();
    assert(!o.begin_frame(false) && log.empty());
    // A drain (GXDrawDone in a tick) while pending: settle, then the deferred begin.
    assert(o.begin_frame(true));
    o.end_frame();
    assert(o.begin_frame(true));
    log.clear();
    o.settle(SettleSite::Drain);
    assert(log==(Log{"settle 5","finish","begin"}) && o.stats.sites[4]==1);
    assert(std::string(twv::vita::settle_site_name(1))=="second_tick" &&
           std::string(twv::vita::settle_site_name(9))=="invalid");
    std::puts("frame_overlap_test: end without present, deferred begin, settle sites, idle count, pending end and unpresentable window passed");
}
}

// --- the FIFO functions (patches/aurora/0009) ---
namespace aurora::gx::fifo {
enum class ProcessingMode { Drain, Inline, Thread };
#if TWV_TEST_THREAD
constexpr auto kProcessingMode=ProcessingMode::Thread;
#else
constexpr auto kProcessingMode=ProcessingMode::Inline;
#endif
struct Mutex {
    unsigned locks=0,unlocks=0;
    void lock(){++locks;}
    void unlock(){++unlocks;}
} sBufferMutex;
// Simulated worker: a wait runs it, which processes everything published (Thread mode only).
template<class T> struct Atomic {
    T value=0; unsigned waits=0;
    T load(std::memory_order) const {return value;}
    void store(T v,std::memory_order){value=v;}
    void wait(T old,std::memory_order);
};
Atomic<uint64_t> sPublished,sProcessed;
template<class T> void Atomic<T>::wait(T old,std::memory_order) {
    assert(kProcessingMode==ProcessingMode::Thread && value==old);
    ++waits; value=sPublished.value;
}
uint64_t sStreamBase=0;
bool sFrameActive=true;
uint32_t sPendingDraws=0;
unsigned wakes=0,inlineRuns=0;
uint64_t clockNow=0;
uint64_t sDrawDoneMark=0,sWaitUs=0;
uint64_t wait_clock() noexcept {return clockNow+=5;}
void wake_worker() noexcept {++wakes;}
void process_to(uint64_t target,std::memory_order) noexcept {++inlineRuns;sProcessed.value=target;}
namespace detail {uint8_t buffer[32];uint8_t* sBufferData=buffer;uint32_t sBufferSize=0;bool sInDisplayList=false;}
#define AURORA_ASSERT(condition,...) assert(condition)
// Extracted from the actual patched production functions by tools/host_tests.sh.
#include "frame_overlap_fifo_functions.inc"

void write(const char* bytes) {
    const auto n=uint32_t(std::strlen(bytes));
    std::memcpy(detail::sBufferData+detail::sBufferSize,bytes,n);detail::sBufferSize+=n;
}
void test() {
    // A frame of 6 bytes with a draw-done after the first 2.
    write("ab");note_draw_done();write("cdef");
    assert(draw_done_mark()==2);
    sPendingDraws=3;
    const uint64_t end=publish_frame();
    assert(end==6 && sPublished.value==6 && sPendingDraws==0);
    if constexpr(kProcessingMode==ProcessingMode::Thread) assert(wakes==1 && sProcessed.value==0);
    else assert(inlineRuns==1 && sProcessed.value==6);
    // A second publish of the same bytes does nothing.
    assert(publish_frame()==6 && (wakes+inlineRuns)==1);
    close_frame();
    assert(!sFrameActive);
    // The draw-done wait: the worker runs (it processes all that is published) and the wait is timed.
    wait_processed(draw_done_mark());
    assert(sProcessed.value==6);
    if constexpr(kProcessingMode==ProcessingMode::Thread) assert(sProcessed.waits==1 && wait_us()==5);
    else assert(sProcessed.waits==0 && wait_us()==0);
    // The next frame's bytes, written between frames: not published, kept by the settle.
    detail::sInDisplayList=true;note_draw_done();detail::sInDisplayList=false; // a display list: no mark
    assert(draw_done_mark()==2);
    write("XY");
    settle_frame(end);
    assert(sStreamBase==6 && detail::sBufferSize==2 && std::memcmp(detail::sBufferData,"XY",2)==0);
    assert(sPublished.value==6 && sPendingDraws==0);
    if constexpr(kProcessingMode==ProcessingMode::Thread) assert(sBufferMutex.locks==1 && sBufferMutex.unlocks==1);
    else assert(sBufferMutex.locks==0);
    // Stream positions continue: a draw-done in the next frame is at base + offset.
    write("Z");note_draw_done();
    assert(draw_done_mark()==9);
    const uint64_t end2=publish_frame();
    assert(end2==9);
    const uint64_t waitsBefore=sProcessed.waits;
    settle_frame(end2);
    assert(sStreamBase==9 && detail::sBufferSize==0);
    if constexpr(kProcessingMode==ProcessingMode::Thread) assert(sProcessed.waits==waitsBefore+1 && wait_us()==10);
    // Already processed: no wait, no time.
    wait_processed(9);
    assert(sProcessed.waits==waitsBefore+(kProcessingMode==ProcessingMode::Thread?1:0));
    std::puts(TWV_TEST_THREAD ? "frame_overlap_test: Thread publish/wake, timed waits, settle keeps the next frame's bytes and positions"
                              : "frame_overlap_test: Inline publish processes at once, no waits, settle keeps the next frame's bytes");
}
}

int main() {
    machine::test();
    aurora::gx::fifo::test();
}
