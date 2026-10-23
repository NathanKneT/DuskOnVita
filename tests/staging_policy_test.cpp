#include "../platform/vita/gfx_vgl/staging_policy.hpp"
#include <cassert>
#include <cstring>
#include <limits>
#include <initializer_list>
using namespace twv::vgl;
int main() {
    constexpr auto vram=VertexWriteClass::Vram,uncached=VertexWriteClass::VerifiedUncachedRam;
    constexpr auto cached=VertexWriteClass::CachedOrExternal,unknown=VertexWriteClass::Unknown;
    constexpr size_t min=VertexStaging::MinBytes,max=VertexStaging::MaxBytes;
    static_assert(should_stage_vertex_output(min,true,vram)); // Legacy remains on.
    static_assert(!should_stage_vertex_output(min,true,uncached)); // New default OFF.
    for(size_t bytes:{size_t(0),min-1,min,min+1,max-1,max,max+1,std::numeric_limits<size_t>::max()}) {
        const bool bounded=bytes>=min && bytes<=max;
        for(bool enabled:{false,true}) {
            assert(should_stage_vertex_output(bytes,true,vram,enabled)==bounded);
            assert(should_stage_vertex_output(bytes,true,uncached,enabled)==(bounded && enabled));
            for(auto target:{cached,unknown,VertexWriteClass(0xffffffffu)})
                assert(!should_stage_vertex_output(bytes,true,target,enabled));
            for(auto target:{vram,uncached,cached,unknown})
                assert(!should_stage_vertex_output(bytes,false,target,enabled));
        }
    }
    // Same bounded CPU scratch, exact output, final GPU destination unchanged.
    alignas(float) uint8_t direct[2048]{},mapped[2048]{};
    VertexStaging scratch;
    auto* cpu=scratch.acquire(sizeof mapped);assert(cpu);
    for(unsigned i=0;i<sizeof mapped/sizeof(float);++i) {
        const float value=float(i)*0.125f-3.0f;
        std::memcpy(direct+i*sizeof(float),&value,sizeof value);
        std::memcpy(cpu+i*sizeof(float),&value,sizeof value);
    }
    std::memcpy(mapped,cpu,sizeof mapped);
    assert(std::memcmp(mapped,direct,sizeof mapped)==0);
    assert(scratch.acquire(sizeof mapped)==cpu && scratch.bytes()==sizeof mapped);
    assert(!scratch.acquire(max+1) && scratch.bytes()==sizeof mapped);
    assert(!scratch.acquire(min-1) && scratch.bytes()==sizeof mapped);
    scratch.clear();assert(!scratch.bytes());
}
