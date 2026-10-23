#include "gfx_vgl/decode_probe.hpp"
#include <cassert>
#include <cstdio>

int main() {
    twv::vgl::DecodeProbe p;
    // One frame in Period is sampled, starting after the Period-th frame end.
    for (unsigned i = 0; i < 3 * p.Period; ++i) {
        const bool sampled = p.sampled;
        p.list_enter(64, sampled ? 1000 : 0);
        if (sampled) p.draw(7);
        p.list_leave(sampled ? 1010 : 0);
        if (sampled) { p.draw(5); p.fifo(30); }
        p.frame_end();
    }
    assert(p.frames == 3 * p.Period && p.sampledFrames == 2 && p.lists == 3 * p.Period);
    assert(p.listBytes == 64 * 3 * p.Period && p.nestedLists == 0 && p.depth == 0);
    // Per sampled frame: list 10 us of which 7 in a draw, one more 5 us draw outside the list.
    assert(p.drawUs == 2 * 12 && p.listUs == 2 * 3 && p.fifoUs == 2 * 30);
    // A nested list counts once in time, and an unmatched leave changes nothing.
    p.sampled = true;
    p.list_enter(8, 100);
    p.list_enter(8, 101);
    p.list_leave(102);
    p.list_leave(110);
    p.list_leave(120);
    assert(p.nestedLists == 1 && p.listUs == 2 * 3 + 10 && p.depth == 0);

    // Per-kind time: the clock is read only when the kind changes, the parts add up to the FIFO time.
    using namespace twv::vgl;
    DecodeProbe q;
    assert(!q.timing() && !q.wants(DecBp));
    q.sampled = q.inFifo = true;
    q.fifo_begin(1000);
    assert(!q.wants(DecOther) && q.wants(DecBp));
    q.mark_at(DecBp, 1004);      // 4 us of loop before the first BP write
    assert(!q.wants(DecBp));     // consecutive BP writes need no clock read
    q.mark_at(DecXf, 1010);
    q.mark_at(DecDrawParse, 1013);
    q.mark_at(DecPipeline, 1020);
    q.mark_at(DecBackend, 1022);
    q.mark_at(DecDrawParse, 1050);
    q.mark_at(DecBp, 1051);
    q.fifo_end(1060);
    const uint64_t expect[DecClassCount] = {4, 6 + 9, 3, 0, 0, 7 + 1, 2, 0, 28};
    uint64_t sum = 0;
    for (unsigned c = 0; c < DecClassCount; ++c) { assert(q.classUs[c] == expect[c]); sum += q.classUs[c]; }
    assert(sum == 60 && q.switches == 7 && DecodeClassNames[DecDrawParse][0] == 'd');
    std::puts("decode_probe_test: sampling period, list time minus nested draws, nesting, unmatched leave and "
              "per-kind time adding up to the FIFO time passed");
}
