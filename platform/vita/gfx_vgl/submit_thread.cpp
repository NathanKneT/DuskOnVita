// SDK domain: the submit thread of --vita-gxm-submit-thread (submit_thread.hpp).
#include "submit_thread.hpp"
#include "runtime/telemetry.hpp"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <atomic>
#include <memory>

namespace twv::gpu::submit {
namespace {
// 32 commands of about 3 KB. On the console on 12 Nov (256 slots), handing a draw over cost the worker about
// 11 us where 3 us was estimated, and the queue was never full. The thread keeps up, so a few
// slots are enough, and 100 KB that is written and read again every few draws stays in the cache
// where 800 KB did not.
constexpr uint32_t Capacity = 32;
constexpr int Priority = 0x10000100;  // the default user priority, as the main thread and the GX worker
constexpr int StackBytes = 512 * 1024;
// Core 3 where the system gives it to a user thread, else core 2 (main: core 0, GX worker:
// core 1; audio, the telemetry writer and the GXM timing thread are on core 2).
constexpr int Core3 = 0x80000;
constexpr int Core2 = 0x40000;  // SCE_KERNEL_CPU_MASK_USER_2
constexpr SceUInt64 SpinUs = 200;  // how long the thread looks for the next command before it sleeps

std::unique_ptr<SubmitQueue> queue;
const TwvGpuBackend* target = nullptr;
SceUID thread = -1, wakeSema = -1, drainSema = -1;
std::atomic<bool> sleeping{false}, draining{false}, stopping{false};
bool verifyMode = false;
// Producer-side counters (the GX worker, and the main thread at frame ends; never the consumer).
uint64_t draws = 0, samplers = 0, drains = 0, drainWaits = 0, fullWaits = 0, wakes = 0;

int thread_main(SceSize, void*) {
    for (;;) {
        while (SubmitCommand* c = queue->front()) {
            if (c->kind == SubmitCommand::Draw) {
                for (unsigned i = 0; c->units.mask >> i; ++i)
                    if (c->units.mask & (1u << i))
                        target->texture_sampler_unit(i, c->units.params[i][0], c->units.params[i][1],
                                                     c->units.params[i][2], c->units.params[i][3]);
                target->draw(c->hasState ? &c->state : nullptr, c->vertices, c->count, c->indices, c->indexCount);
            } else
                target->texture_sampler_unit(c->unit, c->min, c->mag, c->s, c->t);
            queue->pop();
            if (draining.load(std::memory_order_seq_cst) && queue->empty()) sceKernelSignalSema(drainSema, 1);
        }
        if (stopping.load(std::memory_order_seq_cst)) return 0;
        // The worker hands over a draw every 40 to 60 us while a frame is decoded and this thread
        // needs about 20 us for one, so the queue runs empty between draws. Sleeping each time
        // would cost a semaphore signal and a wait per draw (the first Vita3K run made 357,000
        // wake-ups for 208,000 draws): look again for a short while before sleeping.
        {
            const SceUInt64 start = sceKernelGetProcessTimeWide();
            bool arrived = false;
            for (unsigned i = 0; !arrived; ++i) {
                arrived = queue->front() != nullptr;
                if (!arrived && (i & 31) == 31 && sceKernelGetProcessTimeWide() - start > SpinUs) break;
            }
            if (arrived) continue;
        }
        // Sleep only if nothing arrived between the last look and the flag: the producer signals
        // when it sees the flag, and a signal that arrives early just makes the wait return.
        sleeping.store(true, std::memory_order_seq_cst);
        if (!queue->empty()) {
            sleeping.store(false, std::memory_order_seq_cst);
            continue;
        }
        sceKernelWaitSema(wakeSema, 1, nullptr);
        sleeping.store(false, std::memory_order_seq_cst);
    }
}

void wake() {
    if (sleeping.load(std::memory_order_seq_cst)) {
        sceKernelSignalSema(wakeSema, 1);  // an overflow (already signalled) is fine
        ++wakes;
    }
}

SubmitCommand* slot() {
    for (;;) {
        if (SubmitCommand* c = queue->reserve()) return c;
        ++fullWaits;
        wake();
        sceKernelDelayThread(100);
    }
}
}  // namespace

bool start(const TwvGpuBackend& backend, bool verify) {
    if (queue) return true;
    auto created = std::make_unique<SubmitQueue>(Capacity);
    if (!created->ok()) return false;
    wakeSema = sceKernelCreateSema("twv_gpu_submit_wake", 0, 0, 1, nullptr);
    drainSema = sceKernelCreateSema("twv_gpu_submit_drain", 0, 0, 0x7fffffff, nullptr);
    if (wakeSema < 0 || drainSema < 0) {
        twv::vita::telemetryEvent("gfx_submit_thread_error", wakeSema < 0 ? wakeSema : drainSema);
        return false;
    }
    target = &backend;
    verifyMode = verify;
    queue = std::move(created);
    int core = 3;
    thread = sceKernelCreateThread("twv_gpu_submit", thread_main, Priority, StackBytes, 0, Core3, nullptr);
    if (thread < 0) {
        core = 2;
        thread = sceKernelCreateThread("twv_gpu_submit", thread_main, Priority, StackBytes, 0, Core2, nullptr);
    }
    const int started = thread < 0 ? thread : sceKernelStartThread(thread, 0, nullptr);
    if (started < 0) {
        twv::vita::telemetryEvent("gfx_submit_thread_error", started);
        queue.reset();
        return false;
    }
    twv::vita::telemetryEvent("gfx_submit_thread", verify ? 2 : 1);
    twv::vita::telemetryEvent("gfx_submit_thread_core", core);
    return true;
}

bool active() noexcept { return queue != nullptr; }

void drain() {
    if (!queue || queue->empty()) return;
    ++drains;
    draining.store(true, std::memory_order_seq_cst);
    // The thread is usually within a few draws of the end: look for a short while before waiting
    // on the semaphore (console, 12 Nov: four timed waits per frame).
    wake();
    {
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        for (unsigned i = 0; !queue->empty(); ++i)
            if ((i & 31) == 31 && sceKernelGetProcessTimeWide() - start > SpinUs) break;
    }
    while (!queue->empty()) {
        wake();
        SceUInt timeout = 200;  // microseconds: a missed signal costs at most this
        sceKernelWaitSema(drainSema, 1, &timeout);
        ++drainWaits;
    }
    draining.store(false, std::memory_order_seq_cst);
}

void draw(const TwvVitaDrawState* state, const void* vertices, uint32_t count, const uint16_t* indices,
          uint32_t indexCount) {
    SubmitCommand* c = slot();
    c->kind = SubmitCommand::Draw;
    c->vertices = vertices;
    c->indices = indices;
    c->count = count;
    c->indexCount = indexCount;
    c->hasState = state != nullptr;
    c->units.mask = 0;
    if (state) c->state = *state;
    queue->publish();
    ++draws;
    wake();
    if (verifyMode) drain();
}

SubmitCommand* begin_draw() {
    SubmitCommand* c = slot();
    c->kind = SubmitCommand::Draw;
    return c;
}

void end_draw() {
    queue->publish();
    ++draws;
    wake();
    if (verifyMode) drain();
}

void sampler_unit(uint32_t unit, int min, int mag, int s, int t) {
    SubmitCommand* c = slot();
    c->kind = SubmitCommand::SamplerUnit;
    c->unit = unit;
    c->min = min;
    c->mag = mag;
    c->s = s;
    c->t = t;
    queue->publish();
    ++samplers;
    wake();
}

void stop() {
    if (!queue) return;
    drain();
    stopping.store(true, std::memory_order_seq_cst);
    sceKernelSignalSema(wakeSema, 1);
    sceKernelWaitThreadEnd(thread, nullptr, nullptr);
    queue.reset();
}

void report() {
    if (!queue) return;
    twv::vita::telemetryEvent("gfx_submit_draws", static_cast<long long>(draws));
    twv::vita::telemetryEvent("gfx_submit_sampler_units", static_cast<long long>(samplers));
    twv::vita::telemetryEvent("gfx_submit_drains", static_cast<long long>(drains));
    twv::vita::telemetryEvent("gfx_submit_drain_waits", static_cast<long long>(drainWaits));
    twv::vita::telemetryEvent("gfx_submit_full_waits", static_cast<long long>(fullWaits));
    twv::vita::telemetryEvent("gfx_submit_wakes", static_cast<long long>(wakes));
}
}  // namespace twv::gpu::submit
