#include "gfx_vgl/vertex_staging.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
int main() {
    using namespace twv::vgl;
    VertexStaging stage;
    assert(!stage.acquire(0) && !stage.acquire(VertexStaging::MinBytes-1));
    assert(stage.bytes()==0 && !stage.acquire(VertexStaging::MaxBytes+1));
    for(size_t bytes:{size_t(1024),size_t(2049),size_t(4096),size_t(100000),VertexStaging::MaxBytes}) {
        auto* cpu=stage.acquire(bytes);assert(cpu && (uintptr_t(cpu)%alignof(float))==0);
        assert(stage.bytes()>=bytes && stage.bytes()<=VertexStaging::MaxBytes);
        for(size_t i=0;i<bytes;++i)cpu[i]=uint8_t(i*73);
        std::vector<uint8_t> mapped(bytes+32,0xAC);
        std::memcpy(mapped.data()+16,cpu,bytes);
        for(size_t i=0;i<bytes;++i)assert(mapped[i+16]==uint8_t(i*73));
        for(unsigned i=0;i<16;++i)assert(mapped[i]==0xAC && mapped[bytes+16+i]==0xAC);
    }
    // A cache entry owns its copy; the next draw can overwrite/reuse CPU scratch.
    GeometryCache cache(8192);GeometryKey key{};uint8_t indices[8]{};
    auto* cpu=stage.acquire(1024);std::memset(cpu,0x5A,1024);SourceSpan input{indices,8};
    assert(cache.store(0,key,&input,1,cpu,1024,1));
    std::memset(cpu,0xBB,1024);assert(cache.lookup(0,key).data[500]==0x5A);
    stage.clear();assert(stage.bytes()==0 && cache.lookup(0,key).data[0]==0x5A);
    assert(stage.acquire(1024));
    std::puts("vertex_staging_test: bounded growth, aligned stores, copy guards, cache ownership and teardown passed");
}
