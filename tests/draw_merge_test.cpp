// gfx_vgl/draw_merge.hpp (--vita-gxm-draw-merge): the hold in front of the backend. Whatever the
// stream of sampler-unit requests, draws and other calls, the backend must receive every quad and
// every triangle exactly once, in the order given, from the same vertex address, with the same
// draw state and the same sampler-unit requests; verify must send every draw on its own.
#include "gfx_vgl/draw_merge.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace twv::gpu;

namespace {
// One primitive as the backend would draw it: where its vertices are and with what.
struct Primitive {
    uint32_t stateId;
    const unsigned char* at;
    uint32_t vertices;
    DrawUnits units;
    bool operator==(const Primitive& o) const {
        return stateId == o.stateId && at == o.at && vertices == o.vertices && units.same(o.units);
    }
};
// A call as the backend would see it (flush points included, so order across them is checked).
struct Call {
    int kind;  // 0 draw (whole, for strips, fans, indexed, stateless), 1 primitive, 2 other call
    Primitive primitive;
    uint32_t count, indexCount;
    bool operator==(const Call& o) const {
        return kind == o.kind && (kind == 2 || (primitive == o.primitive && count == o.count && indexCount == o.indexCount));
    }
};
void expand(std::vector<Call>& out, const TwvVitaDrawState* state, const void* vertices, uint32_t count,
            uint32_t indexCount, const DrawUnits& units) {
    const auto* at = static_cast<const unsigned char*>(vertices);
    if (!merge_shape(state, vertices, count, indexCount)) {
        out.push_back({0, {state ? state->gpu_vertex : 0xffffffffu, at, count, units}, count, indexCount});
        return;
    }
    const uint32_t per = state->primitive == TwvPrimitiveQuads ? 4 : 3;
    for (uint32_t i = 0; i < count; i += per)
        out.push_back({1, {state->gpu_vertex, at + size_t(i) * state->vertex_layout.stride, per, units}, 0, 0});
}
struct Sink {
    std::vector<Call> calls;
    TwvVitaDrawState held;
    uint32_t submits = 0, reserves = 0, largest = 0;
    TwvVitaDrawState* reserve() {
        ++reserves;
        return &held;
    }
    void submit(const TwvVitaDrawState* state, const void* vertices, uint32_t count, const uint16_t*,
                uint32_t indexCount, const DrawUnits& units) {
        ++submits;
        if (count > largest) largest = count;
        expand(calls, state, vertices, count, indexCount, units);
    }
};
TwvVitaDrawState make_state(uint32_t id, uint32_t primitive, uint32_t stride) {
    TwvVitaDrawState s;
    std::memset(&s, 0, sizeof s);
    s.program = 1 + id % 3;
    s.gpu_vertex = id;  // the test's identity of a state (any field would do)
    s.primitive = primitive;
    s.vertex_layout.stride = stride;
    s.projection[5] = float(id);
    return s;
}
}  // namespace

int main() {
    // Shapes.
    {
        TwvVitaDrawState q = make_state(1, TwvPrimitiveQuads, 16), t = make_state(2, TwvPrimitiveTriangles, 16);
        TwvVitaDrawState strip = make_state(3, TwvPrimitiveStrip, 16), fan = make_state(4, TwvPrimitiveFan, 16);
        TwvVitaDrawState flat = make_state(5, TwvPrimitiveQuads, 0);
        unsigned char v[64] = {};
        assert(merge_shape(&q, v, 4, 0) && merge_shape(&q, v, 8, 0) && !merge_shape(&q, v, 6, 0));
        assert(merge_shape(&t, v, 3, 0) && !merge_shape(&t, v, 4, 0) && !merge_shape(&t, v, 3, 3));
        assert(!merge_shape(&strip, v, 4, 0) && !merge_shape(&fan, v, 4, 0) && !merge_shape(&flat, v, 4, 0));
        assert(!merge_shape(nullptr, v, 4, 0) && !merge_shape(&q, nullptr, 4, 0) && !merge_shape(&q, v, 0, 0));
    }
    // Refusals, one by one.
    {
        TwvVitaDrawState a = make_state(1, TwvPrimitiveQuads, 16), b = a, c = a;
        c.kcolors[3] = 0.5f;  // one float deep inside the state
        unsigned char v[16 * 16] = {};
        DrawUnits u, w;
        u.set(0, 1, 1, 0, 0);
        w.set(0, 1, 0, 0, 0);
        assert(merge_refusal(a, v, 4, u, b, v + 64, 4, u) == MergeNone);
        assert(merge_refusal(a, v, 4, u, c, v + 64, 4, u) == MergeState);
        assert(merge_refusal(a, v, 4, u, b, v + 64, 4, w) == MergeUnits);
        assert(merge_refusal(a, v, 4, u, b, v + 64, 4, DrawUnits{}) == MergeUnits);
        assert(merge_refusal(a, v, 4, u, b, v + 80, 4, u) == MergeApart);
        assert(merge_refusal(a, v, 4, u, b, v, 4, u) == MergeApart);
        assert(merge_refusal(a, v, MergeVertexLimit - 4, u, b, v + size_t(MergeVertexLimit - 4) * 16, 8, u) == MergeSize);
        assert(merge_refusal(a, v, MergeVertexLimit - 4, u, b, v + size_t(MergeVertexLimit - 4) * 16, 4, u) == MergeNone);
        // A request for a unit the other draw did not make is a difference; values of units not
        // requested are not.
        DrawUnits x = u, y = u;
        x.params[5][0] = 7;
        assert(x.same(y));
        y.set(5, 7, 0, 0, 0);
        assert(!x.same(y));
    }
    // A row of four quads becomes one draw; verify sends four.
    for (int verify = 0; verify < 2; ++verify) {
        Sink sink;
        DrawMerger<Sink> merger(sink);
        merger.set_verify(verify != 0);
        TwvVitaDrawState s = make_state(1, TwvPrimitiveQuads, 20);
        std::vector<unsigned char> v(20 * 16);
        for (unsigned i = 0; i < 4; ++i) {
            merger.unit(0, 1, 1, 0, 0);
            merger.draw(&s, v.data() + i * 80, 4, nullptr, 0);
        }
        assert(sink.submits == (verify ? 3u : 0u));  // the last draw is still held
        merger.flush();
        merger.flush();
        assert(sink.submits == (verify ? 4u : 1u) && sink.calls.size() == 4 && merger.draws == 4);
        assert(merger.refused[MergeNone] == 3 && sink.largest == (verify ? 4u : 16u));
        for (unsigned i = 0; i < 4; ++i) assert(sink.calls[i].primitive.at == v.data() + i * 80);
    }
    // Random streams against the plain order.
    std::mt19937 rng(20261111);
    std::vector<TwvVitaDrawState> states;
    for (uint32_t i = 0; i < 6; ++i) states.push_back(make_state(i, i % 3 == 2 ? TwvPrimitiveTriangles : TwvPrimitiveQuads, 12 + 4 * (i % 2)));
    states.push_back(make_state(6, TwvPrimitiveStrip, 16));
    states.push_back(make_state(7, TwvPrimitiveFan, 16));
    std::vector<unsigned char> memory(1 << 20);
    uint64_t merged = 0, total = 0;
    for (int verify = 0; verify < 2; ++verify) {
        for (unsigned round = 0; round < 400; ++round) {
            Sink sink;
            DrawMerger<Sink> merger(sink);
            merger.set_verify(verify != 0);
            std::vector<Call> expected;
            size_t cursor = 0;
            uint32_t stateIndex = rng() % states.size();
            for (unsigned step = 0; step < 300; ++step) {
                const unsigned dice = rng() % 100;
                if (dice < 4) {  // any other backend call
                    merger.flush();
                    sink.calls.push_back({2, {}, 0, 0});
                    expected.push_back({2, {}, 0, 0});
                    continue;
                }
                if (dice < 30) stateIndex = rng() % states.size();
                const TwvVitaDrawState& s = states[stateIndex];
                DrawUnits units;
                const unsigned unitDice = rng() % 10;
                if (unitDice < 8) {
                    merger.unit(0, 1, unitDice == 7 ? 0 : 1, 0, 0);
                    units.set(0, 1, unitDice == 7 ? 0 : 1, 0, 0);
                }
                if (unitDice < 2) {
                    merger.unit(3, 0, 1, 2, 2);
                    units.set(3, 0, 1, 2, 2);
                }
                if (rng() % 8 == 0) cursor += 4 * (1 + rng() % 5);  // something else took memory in between
                const uint32_t per = s.primitive == TwvPrimitiveQuads ? 4 : s.primitive == TwvPrimitiveTriangles ? 3 : 1;
                uint32_t count = per * (1 + rng() % 3) + (s.primitive == TwvPrimitiveStrip ? 3 : 0);
                if (rng() % 40 == 0) count += 1;  // a ragged packet: never merged
                const bool indexed = rng() % 25 == 0;
                const bool stateless = rng() % 60 == 0;
                if (cursor + size_t(count) * s.vertex_layout.stride > memory.size()) cursor = 0;
                const unsigned char* at = memory.data() + cursor;
                cursor += size_t(count) * s.vertex_layout.stride;
                static const uint16_t list[16] = {};
                merger.draw(stateless ? nullptr : &s, at, count, indexed ? list : nullptr, indexed ? 3 : 0);
                expand(expected, stateless ? nullptr : &s, at, count, indexed ? 3 : 0, units);
            }
            merger.flush();
            assert(sink.calls.size() == expected.size());
            for (size_t i = 0; i < expected.size(); ++i) assert(sink.calls[i] == expected[i]);
            assert(sink.largest <= MergeVertexLimit);
            if (verify) assert(sink.submits == merger.draws);
            else assert(sink.submits + merger.refused[MergeNone] == merger.draws);
            merged += merger.refused[MergeNone];
            total += merger.draws;
        }
    }
    assert(merged > total / 20);  // the streams do exercise the merge
    std::printf("draw_merge_test ok: %llu of %llu draws joined the one before\n",
                static_cast<unsigned long long>(merged), static_cast<unsigned long long>(total));
    return 0;
}
