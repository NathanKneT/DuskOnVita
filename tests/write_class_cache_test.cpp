#include "../platform/vita/gfx_vgl/write_class_cache.hpp"
#include <cassert>
using namespace twv::vgl;
int main() {
    assert(write_range_contains(4096,8192,4096,8192));
    assert(write_range_contains(4096,8192,12287,1));
    assert(!write_range_contains(4096,8192,12288,1));
    assert(!write_range_contains(4096,8192,4095,1));
    assert(!write_range_contains(4096,8192,4096,8193));
    assert(!write_range_contains(4096,8192,4096,0));
    assert(!write_range_contains(UINTPTR_MAX-1,8,UINTPTR_MAX-1,1));
    assert(!write_range_contains(0,8192,4096,1));
    PersistentRamWriteClass cache;
    assert(!cache.contains(4096,1,1));
    assert(!cache.remember(4096,64,4096,8192,-1,true,true,true,1));
    assert(!cache.remember(4096,64,4096,8192,0,false,true,true,1));
    assert(!cache.remember(4096,64,4096,8192,0,true,false,true,1));
    assert(!cache.remember(4096,64,4096,8192,0,true,true,false,1));
    assert(!cache.remember(4096,8193,4096,8192,0,true,true,true,1));
    assert(cache.remember(4096,64,4096,8192,0,true,true,true,1));
    assert(cache.contains(8192,4096,1)); // Whole persistent block, not first draw.
    assert(!cache.contains(8192,4097,1));
    assert(!cache.contains(8192,4096,2)); // Backend lifetime identity changed.
    cache.clear();assert(!cache.contains(4096,1,1));
    assert(cache.remember(16384,64,16384,4096,0,true,true,true,2));
    assert(!cache.contains(4096,64,2));
}
