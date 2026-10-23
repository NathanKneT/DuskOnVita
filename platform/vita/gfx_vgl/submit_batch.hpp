#pragma once
#include "gpu_api.h"
#include <cstdint>
#include <cstring>
#include <limits>
namespace twv::vita {
struct SubmitStats {
    uint64_t draws=0, compatible_pairs=0, array_proposal_pairs=0;
    uint64_t barriers=0, invalid=0, cap_breaks=0, runs=0;
    uint32_t longest_run=0;
};
// Diagnostic only: owns one state snapshot, never retains/dereferences geometry.
// Call only behind SUBMIT_OBSERVER. Every external GL mutation is a barrier.
class SubmitObserver {
public:
    static constexpr uint32_t MaxRun=64;
    void reset() { *this=SubmitObserver{}; }
    void barrier() { ++stats_.barriers; live_=false; run_=0; }
    const SubmitStats& stats() const { return stats_; }
    // lifetime_token: nonzero only when caller proves immutable, GPU-mapped
    // backing allocation remains alive through a hypothetical deferred flush.
    // Same token also guarantees the entire contiguous byte range belongs to
    // that one allocation. Zero is normal: compatibility still counted.
    void observe(const TwvVitaDrawState& state, const void* vertices,
                 uint32_t count, const uint16_t* indices, uint32_t index_count,
                 uint64_t lifetime_token=0) {
        ++stats_.draws;
        const uint64_t bytes=uint64_t(count)*state.vertex_layout.stride;
        const uintptr_t address=reinterpret_cast<uintptr_t>(vertices);
        bool valid=vertices && count && state.program && state.vertex_layout.stride &&
            count<=65535 && index_count<=uint32_t(INT32_MAX) && (!index_count || (indices && index_count%3==0)) &&
            (index_count || count%3==0) && bytes<=std::numeric_limits<uintptr_t>::max()-address;
        for (uint32_t a=0;a<TwvVertexAttributeCount;++a) {
            const uint32_t offset=state.vertex_layout.offsets[a];
            if (offset==TwvVertexAttributeAbsent) continue;
            const uint32_t n=state.vertex_layout.components[a];
            if (!n || n>4 || offset>state.vertex_layout.stride ||
                uint64_t(offset)+n*sizeof(float)>state.vertex_layout.stride) valid=false;
        }
        if (!valid) { ++stats_.invalid; live_=false; run_=0; return; }
        const bool equal=live_ && std::memcmp(&previous_,&state,sizeof state)==0;
        if (equal && run_<MaxRun) {
            ++stats_.compatible_pairs; ++run_;
            // Only arrays: indexed draws use separate local index origins and
            // cannot be concatenated without index rewrite or base-vertex support.
            if (!index_count && !indexed_ && token_ && token_==lifetime_token &&
                next_==address && array_vertices_+count<=65535) {
                ++stats_.array_proposal_pairs; array_vertices_+=count;
            } else array_vertices_=count;
        } else {
            if (equal) ++stats_.cap_breaks;
            ++stats_.runs; run_=1; array_vertices_=count;
        }
        if (run_>stats_.longest_run) stats_.longest_run=run_;
        previous_=state; live_=true; next_=address+uintptr_t(bytes);
        token_=lifetime_token; indexed_=index_count!=0;
    }
private:
    TwvVitaDrawState previous_{};
    SubmitStats stats_{};
    uintptr_t next_=0;
    uint64_t token_=0, array_vertices_=0;
    uint32_t run_=0;
    bool live_=false, indexed_=false;
};
}

