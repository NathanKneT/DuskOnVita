#pragma once
#include "vertex_staging.hpp"
#include <cstdint>
namespace twv::vgl {
// Plain integer SDK classification hint, NOT vglMemType or native memory enums.
// Vram preserves the pre-existing VRAM staging route. VerifiedUncachedRam must
// describe the exact requested output range, not merely a vitaGL pool address.
enum class VertexWriteClass : uint32_t {
    Unknown=0,Vram=1,VerifiedUncachedRam=2,CachedOrExternal=3
};
constexpr bool should_stage_vertex_output(size_t bytes,bool cpuBudgetAvailable,
                                         VertexWriteClass target,bool uncachedEnabled=false) {
    if(!cpuBudgetAvailable || bytes<VertexStaging::MinBytes || bytes>VertexStaging::MaxBytes)return false;
    return target==VertexWriteClass::Vram ||
           (uncachedEnabled && target==VertexWriteClass::VerifiedUncachedRam);
}
// Parent integration contract:
// - Existing TWV_VITA_CPU_VERTEX_STAGING compile guard remains authoritative.
// - Add independent --vita-uncached-staging=on/off, default OFF. This flag changes
//   only the second accepted class; never call vglUseCachedMem to implement it.
// - Keep geometryCache.budget()!=0 as existing CPU headroom gate, and call the
//   SAME VertexStaging::acquire(bytes). Failed acquire returns original GPU output.
// - Decode into returned CPU scratch, then existing memcpy(mapped,cpu,bytes)
//   BEFORE draw submission. Never bind CPU staging itself or retain it for GPU.
// - Same1KiB/2MiB thresholds and existing single staging allocation/capacity.
//   No new arena, per-draw allocation, source/PN/state/frame-order changes.
// SDK C ABI proposal: uint32_t twv_vgl_vertex_write_class(const void*,size_t).
// Preserve current vgl_mem_get_type_by_addr==VGL_MEM_VRAM -> Vram behavior.
// To add VerifiedUncachedRam, require VGL_MEM_RAM and native verification:
// SceKernelMemBlockInfo info{}; info.size=sizeof(info);
// sceKernelGetMemBlockInfoByRange(ptr,bytes,&info)==0;
// native info.type==SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
// info.access includes SCE_KERNEL_MEMORY_ACCESS_W; check ptr>=mappedBase,
// offset<=mappedSize and bytes<=mappedSize-offset without pointer overflow.
// Negative query, cached RAM, EXTERNAL/ALL/other classifications -> Unknown/Other.
// The query proves native CPU memory type/range, NOT GXM mapping. Call this only
// for the EXISTING scratch allocator's GPU output; do not broaden ownership or
// infer that arbitrary matching pointers are safe GPU allocations. A per-pool
// native-type memo is optional only after exact-range ownership is established;
// do not perform a native syscall per vertex or assume cachedness from pool ID.
}
