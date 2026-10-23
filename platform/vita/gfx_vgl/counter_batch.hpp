#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#ifndef TWV_VITA_BATCH_COUNTERS
#define TWV_VITA_BATCH_COUNTERS 1
#endif

namespace twv::vgl {
// Existing receipt names are a compatibility contract. These IDs are only for
// the frequently updated counters; dynamic/rare names keep the ordered map.
enum class CounterId : uint8_t {
    GpuMatrixSlots,GpuMatrixSlotsFull,GpuActiveLights,GpuFullLights,GpuLitDraws,
    GpuMatrixPrefixDraws,DrawsSubmitted,VerticesSubmitted,
    GpuVertexDraws,CpuVertexDraws,GpuVertexVertices,CpuVertexVertices,
    GpuTexgenDraws,CpuTexgenDraws,GpuTexgenVertices,CpuTexgenVertices,
    VertexCpuStagedBytes,VertexDirectDecodeBytes,GeometryCacheHitVertices,
    GeometryCacheValidationPass,
    FallbackIndirectDraw,FallbackFogRangeDraw,FallbackLogicBlendDraw,VertexCullDraws,
    ShadowImageCalls,ShadowImageSkippedCalls,ShadowImageSkippedModels,ShadowImageRestoredPackets,
    VertexPlannedFetchVertices,VertexReferenceFetchVertices,VertexCpuStagingFallback,Count
};
inline constexpr std::array<const char*,size_t(CounterId::Count)> CounterNames={
    "gpu_matrix_slots","gpu_matrix_slots_full","gpu_active_lights","gpu_full_lights","gpu_lit_draws",
    "gpu_matrix_prefix_draws","draws_submitted","vertices_submitted",
    "gpu_vertex_draws","cpu_vertex_draws","gpu_vertex_vertices","cpu_vertex_vertices",
    "gpu_texgen_draws","cpu_texgen_draws","gpu_texgen_vertices","cpu_texgen_vertices",
    "vertex_cpu_staged_bytes","vertex_direct_decode_bytes","geometry_cache_hit_vertices",
    "geometry_cache_validation_pass",
    "fallback_indirect_draw","fallback_fog_range_draw","fallback_logic_blend_draw","vertex_cull_draws",
    "shadow_image_calls","shadow_image_skipped_calls","shadow_image_skipped_models","shadow_image_restored_packets",
    "vertex_planned_fetch_vertices","vertex_reference_fetch_vertices","vertex_cpu_staging_fallback"
};
static_assert(CounterNames.size()<=32);

// Single serialized FIFO owner, as with the preceding counter map. Preserve
// zero updates as seen keys and full unsigned64 sums between report boundaries.
class CounterBatch {
public:
    bool add(CounterId id,uint32_t amount) noexcept {
        const size_t slot=size_t(id);
        if(slot>=CounterNames.size())return false;
        totals_[slot]+=amount;
        seen_|=uint32_t(1)<<slot;
        return true;
    }
    template<class Merge> void flush(Merge&& merge) {
        const uint32_t seen=seen_;seen_=0;
        for(size_t i=0;i<CounterNames.size();++i)if(seen&(uint32_t(1)<<i)) {
            const auto delta=totals_[i];totals_[i]=0;
            merge(CounterNames[i],delta);
        }
    }
private:
    std::array<uint64_t,CounterNames.size()> totals_{};
    uint32_t seen_=0;
};
static_assert(sizeof(CounterBatch)<=256);
}
