#include "gfx_vgl/cache_budget.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include <cassert>
#include <cstdio>
static bool allocationFails=false;
static size_t providerBytes=0;
static void* residentAllocate(size_t bytes) {
    if(allocationFails)return nullptr;
    providerBytes+=bytes;return new uint8_t[bytes];
}
static void residentRelease(void* ptr) {
    // Regression fixtures use one-MiB outputs only.
    assert(providerBytes>=twv::vgl::CacheMiB);
    providerBytes-=twv::vgl::CacheMiB;delete[] static_cast<uint8_t*>(ptr);
}
int main() {
    using namespace twv::vgl;
    const uint32_t heap=160*CacheMiB;
    assert(geometry_cache_budget(8*CacheMiB,96*CacheMiB,heap,true)==12*CacheMiB);
    assert(geometry_cache_budget(8*CacheMiB,112*CacheMiB,heap,true)==8*CacheMiB);
    assert(geometry_cache_budget(8*CacheMiB,120*CacheMiB,heap,true)==0);
    assert(geometry_cache_budget(0,160*CacheMiB,heap,true)==0);
    assert(geometry_cache_budget(12*CacheMiB,0,heap,false)==8*CacheMiB);
    // Total12MiB = CPU4MiB + GPU8MiB at the48MiB reserve boundary.
    const auto residentCap=geometry_cache_budget(4*CacheMiB,112*CacheMiB,heap,true);
    assert(residentCap==4*CacheMiB);
    // Worst-case replacement uses CPU for the entire returned total cap.
    assert(112*CacheMiB-4*CacheMiB+residentCap<=heap-CpuHeapReserve);
    for(uint32_t used=48*CacheMiB;used<=heap;used+=11777) {
        const uint32_t current=8*CacheMiB;
        const auto budget=geometry_cache_budget(current,used,heap,true);
        assert(budget<=GeometryCacheMax && budget%CacheMiB==0);
        if(budget)assert(int64_t(used)-current+budget<=int64_t(heap)-CpuHeapReserve);
    }
    uint8_t source[32]{},vertices[32]{};SourceSpan span{source,32};GeometryKey key{};
    GeometryCache cache(128);
    assert(cache.store(0,key,&span,1,vertices,32));
    cache.set_budget(32);assert(cache.bytes()==0 && cache.stats.memoryTrims==1);
    cache.set_budget(0);assert(cache.budget()==0 && cache.bytes()==0);
    cache.set_budget(128);assert(cache.store(0,key,&span,1,vertices,32));
    assert(cache.lookup(0,key).data && cache.source_bytes()==32 && cache.bytes()==64);
    assert(cache.cpu_bytes()==cache.bytes());
    {
        static uint8_t bigSource[CacheMiB/2]{},bigOutput[CacheMiB]{};
        SourceSpan large{bigSource,sizeof bigSource};
        GeometryCache mixed(12*CacheMiB);
        mixed.set_output_allocator(residentAllocate,residentRelease);
        for(unsigned i=0;i<8;++i) {
            GeometryKey k{};k.pipeline=i;
            assert(mixed.store(i,k,&large,1,bigOutput,sizeof bigOutput));
        }
        assert(mixed.bytes()==12*CacheMiB && mixed.cpu_bytes()==4*CacheMiB);
        assert(mixed.stats.residentBytes==8*CacheMiB && providerBytes==8*CacheMiB);
        const auto cap=geometry_cache_budget(uint32_t(mixed.cpu_bytes()),112*CacheMiB,heap,true);
        mixed.set_budget(cap);assert(mixed.bytes()<=4*CacheMiB);
        mixed.clear();assert(mixed.cpu_bytes()==0 && mixed.bytes()==0 && providerBytes==0);
        // Provider/deleter changes plus all-CPU fallback cannot exceed the cap.
        allocationFails=true;
        for(unsigned i=0;i<2;++i) {
            GeometryKey k{};k.pipeline=i;
            assert(mixed.store(i,k,&large,1,bigOutput,sizeof bigOutput));
        }
        assert(mixed.cpu_bytes()==mixed.bytes() && mixed.cpu_bytes()<=cap);
        assert(112*CacheMiB-4*CacheMiB+mixed.cpu_bytes()<=heap-CpuHeapReserve);
        mixed.clear();assert(!mixed.cpu_bytes() && !mixed.stats.residentBytes);
        allocationFails=false;
    }
    std::puts("cache_budget_test: allocator headroom/reserve, quantized limits, unknown stats, pressure trimming and re-admission passed");
}
