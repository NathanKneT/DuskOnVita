#pragma once
#include <cstdint>

namespace twv::vgl {
constexpr uint32_t CacheMiB=1024*1024;
constexpr uint32_t GeometryCacheMax=12*CacheMiB;
constexpr uint32_t GeometryCacheUnknown=8*CacheMiB;
constexpr uint32_t CpuHeapReserve=48*CacheMiB;
// currentCpuBytes includes only snapshots and CPU-owned outputs counted in used.
// GPU-resident output is outside newlib and must never be reclaimable CPU credit.
// Return a conservative TOTAL payload cap: every future output can fall back to
// CPU storage while retaining the reserve for game/shader/compiler/UI work.
// Whole-MiB steps avoid budget churn from small transient allocator variations.
constexpr uint32_t geometry_cache_budget(uint32_t currentCpuBytes,uint32_t used,uint32_t heap,bool valid) {
    if(!valid)return GeometryCacheUnknown;
    const int64_t available=int64_t(currentCpuBytes)+int64_t(heap)-CpuHeapReserve-int64_t(used);
    if(available<=0)return 0;
    const auto capped=available>GeometryCacheMax?GeometryCacheMax:uint32_t(available);
    return capped/CacheMiB*CacheMiB;
}
}
