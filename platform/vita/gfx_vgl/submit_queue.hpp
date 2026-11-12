#pragma once
// --vita-gxm-submit-thread (gfx_vgl/submit_thread.cpp): the queue between the GX worker, which
// decodes GX commands and builds each draw, and the submit thread, which makes the backend's draw
// calls. One producer, one consumer, no lock. A command is taken off the queue only after it has
// been executed, so "empty" means that everything handed over has been drawn.
#include "draw_merge.hpp"
#include "gpu_api.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>

namespace twv::gpu {
struct SubmitCommand {
    enum : uint32_t { Draw = 1, SamplerUnit = 2 };
    uint32_t kind = 0;
    // Draw: the backend's draw arguments. The state is copied; vertices and indices point into
    // memory that stays valid until the frame ends (the frame's ring, resident memory).
    const void* vertices = nullptr;
    const uint16_t* indices = nullptr;
    uint32_t count = 0, indexCount = 0;
    bool hasState = false;
    // SamplerUnit: the backend's texture_sampler_unit arguments.
    uint32_t unit = 0;
    int min = 0, mag = 0, s = 0, t = 0;
    // Draw, with --vita-gxm-draw-merge: the sampler-unit requests that belong to this draw, made
    // just before it (without the merge they travel as SamplerUnit commands and the mask is 0).
    DrawUnits units;
    TwvVitaDrawState state;
};

class SubmitQueue {
public:
    // capacity: a power of two.
    explicit SubmitQueue(uint32_t capacity)
        : slots_(new (std::nothrow) SubmitCommand[capacity]), capacity_(capacity), mask_(capacity - 1) {}
    bool ok() const noexcept { return slots_ != nullptr && capacity_ && !(capacity_ & mask_); }
    // Producer: the next free slot, or nullptr while the queue is full; publish() hands it over.
    SubmitCommand* reserve() noexcept {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= capacity_) return nullptr;
        return &slots_[head & mask_];
    }
    void publish() noexcept { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst); }
    // Consumer: the oldest command, or nullptr; pop() after executing it.
    SubmitCommand* front() noexcept {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return nullptr;
        return &slots_[tail & mask_];
    }
    void pop() noexcept { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst); }
    // Either side: nothing handed over that has not been executed.
    bool empty() const noexcept {
        return tail_.load(std::memory_order_seq_cst) == head_.load(std::memory_order_seq_cst);
    }
    uint32_t published() const noexcept { return head_.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<SubmitCommand[]> slots_;
    uint32_t capacity_, mask_;
    std::atomic<uint32_t> head_{0}, tail_{0};
};
}  // namespace twv::gpu
