#include "resident_geometry_api.h"
#include "resident_geometry.hpp"
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/gxm.h>
#include <vitaGL.h>
// Exported global in pinned vitaGL source/gxm.c. SDK-domain only, never in host headers.
extern "C" SceGxmContext* gxm_context;
// Pinned gxm.c65 and582, declaration shared.h1223. No private struct layout.
extern "C" GLboolean dirty_framebuffer;
extern "C" void scene_reset(void);
extern "C" void twv_vgl_submit_barrier(void);
namespace {
twv::vgl::ResidentGeometryArena arena;
SceUID block=-1;
void* backing=nullptr;
uint64_t completedWaits=0,waitTimeUs=0;
bool complete() {
    if(!gxm_context)return false;
    twv_vgl_submit_barrier();
    const uint64_t begin=sceKernelGetProcessTimeWide();
    // Pinned glFinish clears scene_not_flushed, but later draws do not set it.
    // Force its actual end/begin-scene branch, including vitaGL bookkeeping,
    // before waiting. Neither this path nor native Finish rewinds scratch or
    // uniform pools; pending current-draw allocations remain valid.
    twv::vgl::finish_resident_geometry([] {
        dirty_framebuffer=GL_TRUE;
        scene_reset();
    },[] {
        // VitaSDK declares this blocking primitive void (no error channel).
        sceGxmFinish(gxm_context);
    });
    ++completedWaits;waitTimeUs+=sceKernelGetProcessTimeWide()-begin;
    return true;
}
}
extern "C" int twv_vgl_resident_init(void) {
    if(backing)return 1;
    block=sceKernelAllocMemBlock("twv_resident_geometry",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                               TWV_RESIDENT_GEOMETRY_RESERVE,nullptr);
    if(block<0)return 0;
    void* memory=nullptr;
    if(sceKernelGetMemBlockBase(block,&memory)<0 || !memory) {
        sceKernelFreeMemBlock(block);block=-1;return 0;
    }
    if(sceGxmMapMemory(memory,TWV_RESIDENT_GEOMETRY_RESERVE,SCE_GXM_MEMORY_ATTRIB_READ)<0) {
        sceKernelFreeMemBlock(block);block=-1;return 0;
    }
    if(!arena.init(memory,complete)) {
        sceGxmUnmapMemory(memory);sceKernelFreeMemBlock(block);block=-1;return 0;
    }
    backing=memory;return 1;
}
extern "C" int twv_vgl_resident_available(void){return backing && gxm_context;}
extern "C" int twv_vgl_resident_shutdown(void) {
    if(!backing)return 1;
    if(!arena.shutdown())return 0; // Fail closed: backing remains mapped, never reused.
    if(sceGxmUnmapMemory(backing)<0)return 0;
    sceKernelFreeMemBlock(block);block=-1;backing=nullptr;return 1;
}
extern "C" void* twv_vgl_resident_alloc(size_t bytes){return arena.allocate(bytes);}
extern "C" void twv_vgl_resident_release(void* ptr){arena.release(ptr);}
extern "C" void twv_vgl_resident_get_stats(twv_resident_geometry_stats* out) {
    if(!out)return;
    const auto& s=arena.stats;
    *out={s.allocations,s.fallbacks,s.releases,s.collections,s.completionFailures,
          s.liveBytes,s.retiredBytes,s.peakBytes,s.scannedUnits,completedWaits,waitTimeUs};
}
