#include "../platform/vita/gfx_vgl/staging_policy.hpp"
#include "../platform/vita/gfx_vgl/write_class_cache.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
// SDK field names/types from pinned image sysmem.h. Host pointer width differs;
// this tests production control flow, not the ARM ABI or syscall implementation.
using SceSize=uint32_t;
enum SceKernelMemBlockType {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW=0x0c20d060,
                          SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE=0x0c208060};
struct SceKernelMemBlockInfo {SceSize size;void* mappedBase;SceSize mappedSize;
    int memoryType;uint32_t access;SceKernelMemBlockType type;};
constexpr uint32_t SCE_KERNEL_MEMORY_ACCESS_W=2;
enum vglMemType {VGL_MEM_VRAM,VGL_MEM_RAM,VGL_MEM_PHYCONT,VGL_MEM_BUDGET,VGL_MEM_EXTERNAL,VGL_MEM_ALL};
static bool ready=true;
static twv::vgl::PersistentRamWriteClass vertexWriteClassCache;
static uint64_t vertexWriteOwner=1,vertexWriteQueries=0,vertexWriteHits=0,vertexWriteErrors=0;
static int32_t vertexWriteLastError=0;
static unsigned syscallCount=0,events=0;
static int nativeResult=0;
static uintptr_t nativeBase=0x10000,badEndpoint=0;
static SceSize nativeSize=0x20000;
static uint32_t nativeAccess=2;
static auto nativeType=SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
static long long loggedError=0;
static void* pointer(uintptr_t value){return reinterpret_cast<void*>(value);}
extern "C" vglMemType vgl_mem_get_type_by_addr(void* address) {
    const auto p=reinterpret_cast<uintptr_t>(address);
    if(p==badEndpoint)return VGL_MEM_EXTERNAL;
    if(p>=0x10000 && p<0x30000)return VGL_MEM_RAM;
    if(p>=0x40000 && p<0x50000)return VGL_MEM_VRAM;
    if(p>=0x60000 && p<0x70000)return static_cast<vglMemType>(-1);
    return VGL_MEM_EXTERNAL;
}
static int sceKernelGetMemBlockInfoByRange(void*,SceSize,SceKernelMemBlockInfo* info) {
    ++syscallCount;assert(info->size==sizeof(*info));
    if(nativeResult)return nativeResult;
    info->mappedBase=pointer(nativeBase);info->mappedSize=nativeSize;
    info->access=nativeAccess;info->type=nativeType;return 0;
}
namespace twv::vita {
void telemetryEvent(const char* name,long long value) {
    ++events;if(!std::strcmp(name,"gfx_vertex_write_class_last_error"))loggedError=value;
}
}
#include "write_class_native_fixture.inc"
static void reset() {
    extracted_shutdown_cache_reset();
    ready=true;syscallCount=events=0;nativeResult=0;nativeBase=0x10000;
    nativeSize=0x20000;badEndpoint=0;nativeAccess=2;
    nativeType=SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    vertexWriteQueries=vertexWriteHits=vertexWriteErrors=0;vertexWriteLastError=0;
}
int main() {
    reset();
    assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==2 && syscallCount==1);
    assert(twv_vgl_vertex_write_class(pointer(0x20000),4096)==2 && syscallCount==1);
    assert(vertexWriteQueries==1 && vertexWriteHits==1);
    extracted_shutdown_cache_reset();
    assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==2 && syscallCount==2);
    ready=false;assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0 && syscallCount==2);
    reset();nativeResult=static_cast<int32_t>(0x80020005u);
    assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0);
    assert(vertexWriteLastError==nativeResult && vertexWriteErrors==1);
    twv_vgl_vertex_write_class_report();assert(events==4 && loggedError==nativeResult);
    nativeResult=0;assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==2 && syscallCount==2);
    for(auto endpoint:{uintptr_t(0x10000),uintptr_t(0x2ffff)}) {
        reset();badEndpoint=endpoint;
        assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0);
        assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0 && syscallCount==2);
    }
    reset();nativeAccess=1;assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0);
    reset();nativeType=SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0);
    reset();nativeSize=0x1000;assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0);
    reset();nativeBase=UINTPTR_MAX-1;nativeSize=8192;
    assert(twv_vgl_vertex_write_class(pointer(0x11000),1024)==0);
    reset();assert(twv_vgl_vertex_write_class(pointer(0x2ffff),1)==2);
    reset();assert(twv_vgl_vertex_write_class(pointer(0x2ffff),2)==0);
    reset();
    assert(twv_vgl_vertex_write_class(pointer(0x40000),1024)==1 && syscallCount==0);
    assert(twv_vgl_vertex_write_class(pointer(0x80000),1024)==3 && syscallCount==0);
    assert(twv_vgl_vertex_write_class(pointer(0x60000),1024)==3 && syscallCount==0);
    assert(twv_vgl_vertex_write_class(nullptr,1024)==0);
    assert(twv_vgl_vertex_write_class(pointer(0x11000),0)==0);
    assert(twv_vgl_vertex_write_class(pointer(UINTPTR_MAX-1),8)==0);
    if(SIZE_MAX>UINT32_MAX)
        assert(twv_vgl_vertex_write_class(pointer(0x11000),size_t(UINT32_MAX)+1)==0);
    assert(syscallCount==0);
}
