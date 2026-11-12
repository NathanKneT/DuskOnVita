#pragma once
// --vita-gxm-draw-merge=off|on|verify (GXM backend; gfx_vgl/gpu_dispatch.cpp). The game draws many
// quads and triangle lists one packet at a time with nothing changed between them (HUD glyphs,
// rows of identical quads): section 18 of counted 126 such draws per
// frame in Ordon centre. With the switch on, the dispatch layer holds each quad or triangle-list
// draw back until the next call. If that call is a draw of the same kind whose draw state is the
// same byte for byte, whose sampler-unit requests are the same, and whose vertices start exactly
// where the held draw's vertices end, the held draw simply grows by those vertices: the backend
// then draws the same triangles, from the same bytes, in the same order, in one call instead of
// two. Any other call first sends the held draw on, so the backend sees every call in the order
// it was made. verify: the same hold and the same decisions, counted, but every draw is still
// sent on its own (a picture that differs between on and verify is a merge fault; one that
// differs between verify and off is a fault of the hold itself).
// SDK-free; host-tested in tests/draw_merge_test.cpp.
#include "gpu_api.h"
#include <cstdint>
#include <cstring>

namespace twv::gpu {
// The sampler-unit requests made for one draw (twv_vgl_texture_sampler_unit, units 0..7).
struct DrawUnits {
    uint32_t mask = 0;
    int params[8][4] = {};  // min, mag, s, t
    void set(uint32_t unit, int min, int mag, int s, int t) noexcept {
        mask |= 1u << unit;
        params[unit][0] = min;
        params[unit][1] = mag;
        params[unit][2] = s;
        params[unit][3] = t;
    }
    bool same(const DrawUnits& other) const noexcept {
        if (mask != other.mask) return false;
        for (unsigned i = 0; i < 8; ++i)
            if ((mask & (1u << i)) && std::memcmp(params[i], other.params[i], sizeof params[i]) != 0) return false;
        return true;
    }
};

// Why a draw was not added to the held one (None: it was).
enum MergeRefusal : uint32_t { MergeNone, MergeShape, MergeState, MergeUnits, MergeApart, MergeSize, MergeRefusalCount };
// A merged draw stays far below the backend's own limits (65,536 vertices, 16,384 quads).
constexpr uint32_t MergeVertexLimit = 4096;

// Draws that can be held: whole quads, or whole listed triangles, without an index list. Strips
// and fans cannot be joined, and an index list counts from its own draw's first vertex.
inline bool merge_shape(const TwvVitaDrawState* state, const void* vertices, uint32_t count,
                        uint32_t indexCount) noexcept {
    if (!state || !vertices || !count || indexCount || !state->vertex_layout.stride) return false;
    if (state->primitive == TwvPrimitiveQuads) return count % 4 == 0;
    if (state->primitive == TwvPrimitiveTriangles) return count % 3 == 0;
    return false;
}

// Both draws have passed merge_shape.
inline MergeRefusal merge_refusal(const TwvVitaDrawState& held, const void* heldVertices, uint32_t heldCount,
                                  const DrawUnits& heldUnits, const TwvVitaDrawState& next, const void* vertices,
                                  uint32_t count, const DrawUnits& units) noexcept {
    if (held.program != next.program || held.primitive != next.primitive ||
        std::memcmp(&held, &next, sizeof next) != 0)
        return MergeState;
    if (!heldUnits.same(units)) return MergeUnits;
    if (static_cast<const unsigned char*>(heldVertices) + size_t(heldCount) * held.vertex_layout.stride != vertices)
        return MergeApart;
    if (heldCount + count > MergeVertexLimit) return MergeSize;
    return MergeNone;
}

// The hold. Sink:
//   TwvVitaDrawState* reserve()   storage for the held draw's state, stable until the next submit
//   void submit(const TwvVitaDrawState* state, const void* vertices, uint32_t count,
//               const uint16_t* indices, uint32_t indexCount, const DrawUnits& units)
//                                 the sampler-unit requests, then the draw; state is the pointer
//                                 reserve() returned for a held draw, the caller's otherwise
template <class Sink>
class DrawMerger {
public:
    explicit DrawMerger(Sink& sink) : sink_(sink) {}
    void set_verify(bool verify) noexcept { verify_ = verify; }
    void unit(uint32_t unit, int min, int mag, int s, int t) noexcept { next_.set(unit, min, mag, s, t); }
    void draw(const TwvVitaDrawState* state, const void* vertices, uint32_t count, const uint16_t* indices,
              uint32_t indexCount) {
        const DrawUnits units = next_;
        next_.mask = 0;
        ++draws;
        if (!merge_shape(state, vertices, count, indexCount)) {
            if (held_) ++refused[MergeShape];
            flush();
            sink_.submit(state, vertices, count, indices, indexCount, units);
            return;
        }
        if (held_) {
            const MergeRefusal why =
                merge_refusal(*heldState_, heldVertices_, heldCount_, heldUnits_, *state, vertices, count, units);
            ++refused[why];
            if (why == MergeNone && !verify_) {
                heldCount_ += count;
                return;
            }
            flush();
        }
        heldState_ = sink_.reserve();
        *heldState_ = *state;
        heldVertices_ = vertices;
        heldIndices_ = indices;
        heldCount_ = count;
        heldUnits_ = units;
        held_ = true;
    }
    // Before any call that is not a draw or a sampler-unit request.
    void flush() {
        if (!held_) return;
        held_ = false;
        sink_.submit(heldState_, heldVertices_, heldCount_, heldIndices_, 0, heldUnits_);
    }
    // draws: all draws seen. refused[MergeNone]: draws that joined the held one (verify: that
    // would have). The other entries: a held draw was there and the next one could not join it.
    uint64_t draws = 0, refused[MergeRefusalCount] = {};

private:
    Sink& sink_;
    bool verify_ = false, held_ = false;
    TwvVitaDrawState* heldState_ = nullptr;
    const void* heldVertices_ = nullptr;
    const uint16_t* heldIndices_ = nullptr;
    uint32_t heldCount_ = 0;
    DrawUnits heldUnits_, next_;
};
}  // namespace twv::gpu
