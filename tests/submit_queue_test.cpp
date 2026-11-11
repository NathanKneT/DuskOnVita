// gfx_vgl/submit_queue.hpp (--vita-gxm-submit-thread): one producer and one consumer on real
// threads. Every command arrives once, in order, with the bytes it was given, and the queue is
// only empty once the consumer has executed everything handed over.
#include "gfx_vgl/submit_queue.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>

using twv::gpu::SubmitCommand;
using twv::gpu::SubmitQueue;

int main() {
    assert(!SubmitQueue(0).ok() && !SubmitQueue(3).ok() && SubmitQueue(4).ok());
    // Single thread: capacity, order, wrap-around.
    {
        SubmitQueue q(4);
        assert(q.empty() && !q.front());
        for (unsigned round = 0; round < 10; ++round) {
            for (unsigned i = 0; i < 4; ++i) {
                SubmitCommand* c = q.reserve();
                assert(c);
                c->kind = SubmitCommand::Draw;
                c->count = round * 4 + i;
                q.publish();
            }
            assert(!q.reserve() && !q.empty());
            for (unsigned i = 0; i < 4; ++i) {
                assert(q.front() && q.front()->count == round * 4 + i && !q.empty());
                q.pop();
            }
            assert(q.empty() && !q.front());
        }
    }
    // Two threads. The consumer checks each command; "executed" counts them only after the check,
    // as the submit thread pops only after the backend call returned.
    constexpr uint32_t Total = 400000;
    SubmitQueue q(256);
    std::atomic<uint32_t> executed{0};
    std::atomic<bool> stop{false};
    std::thread consumer([&] {
        uint32_t next = 0;
        while (!stop.load()) {
            SubmitCommand* c = q.front();
            if (!c) {
                std::this_thread::yield();
                continue;
            }
            if (next % 5 == 4) {
                assert(c->kind == SubmitCommand::SamplerUnit && c->unit == next % 8 && c->min == int(next));
            } else {
                assert(c->kind == SubmitCommand::Draw && c->count == next && c->indexCount == ~next && c->hasState);
                assert(c->state.program == next && c->state.viewport[3] == int(next % 977));
                float expected;
                const uint32_t bits = next * 2654435761u;
                std::memcpy(&expected, &bits, 4);
                assert(std::memcmp(&c->state.texgen_rows[48 * 4 - 1], &expected, 4) == 0);
            }
            executed.store(next + 1);  // before pop: empty() must imply executed
            q.pop();
            ++next;
        }
    });
    TwvVitaDrawState state{};
    uint32_t fullWaits = 0, drains = 0;
    for (uint32_t n = 0; n < Total; ++n) {
        SubmitCommand* c;
        while (!(c = q.reserve())) {
            ++fullWaits;
            std::this_thread::yield();
        }
        if (n % 5 == 4) {
            c->kind = SubmitCommand::SamplerUnit;
            c->unit = n % 8;
            c->min = int(n);
        } else {
            state.program = n;
            state.viewport[3] = int(n % 977);
            const uint32_t bits = n * 2654435761u;
            std::memcpy(&state.texgen_rows[48 * 4 - 1], &bits, 4);
            c->kind = SubmitCommand::Draw;
            c->count = n;
            c->indexCount = ~n;
            c->hasState = true;
            c->state = state;
        }
        q.publish();
        if (n % 1009 == 0) {  // a drain: once empty, everything published has been executed
            while (!q.empty()) std::this_thread::yield();
            assert(executed.load() == n + 1);
            ++drains;
        }
    }
    while (!q.empty()) std::this_thread::yield();
    assert(executed.load() == Total && q.published() == Total);
    stop.store(true);
    consumer.join();
    std::printf("submit_queue_test: %u commands in order, %u drains, %u waits on a full queue\n", Total, drains,
                fullWaits);
    return 0;
}
