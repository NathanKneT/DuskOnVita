#include "../platform/vita/gfx_vgl/geometry_cache.hpp"
#include "../platform/vita/gfx_vgl/resident_geometry.hpp"
#include <cassert>
#include <cstring>
using namespace twv::vgl;
alignas(256) static uint8_t memory[8*1024*1024];
static ResidentGeometryArena arena;
static bool completed=true;
static unsigned waits=0,oldReleases=0;
static bool finish(){++waits;return completed;}
static void* alloc(size_t n){return arena.allocate(n);}
static void release(void* p){++oldReleases;arena.release(p);}
static void* fail(size_t){return nullptr;}
int main() {
    // Model the pinned stale-flush-flag case: draw after finish, same scene state.
    // A native wait alone cannot certify commands still in an unsubmitted scene.
    unsigned pending=1,submitted=0,done=0,boundaries=0;
    bool sceneNotFlushed=true;
    auto boundary=[&] {submitted+=pending;pending=0;++boundaries;sceneNotFlushed=true;};
    auto wait=[&] {assert(!pending);done=submitted;sceneNotFlushed=false;};
    finish_resident_geometry(boundary,wait);
    assert(done==1 && !sceneNotFlushed);
    pending=1; // New draw does not mark pinned vitaGL's flag.
    finish_resident_geometry(boundary,wait);
    assert(done==2 && boundaries==2 && !pending);
    assert(arena.init(memory,finish));
    uint8_t root[4]={1,2,3,4},array[4]={5,6,7,8},output[16]={9};
    SourceSpan spans[]={{root,sizeof root},{array,sizeof array}};
    GeometryKey key{};
    GeometryCache cache(1024);
    cache.set_output_allocator(alloc,release);
    assert(cache.store(0,key,spans,2,output,sizeof output,1));
    auto hit=cache.lookup(0,key);
    assert(hit.resident && !std::memcmp(hit.data,output,sizeof output));
    auto ptr=hit.data;
    assert(cache.cpu_bytes()==sizeof root+sizeof array);
    assert(cache.bytes()-cache.cpu_bytes()==sizeof output);
    cache.set_output_allocator(fail,release); // Existing deleter stays attached.
    root[0]^=1;array[0]^=1;
    auto before=cache.stats.validationSpans;
    assert(!cache.lookup(0,key).data);
    assert(cache.stats.validationSpans==before+1); // Raw root rejects before arrays.
    assert(oldReleases==1 && arena.stats.retiredBytes==256);
    assert(cache.cpu_bytes()==0 && cache.bytes()==0);
    assert(!std::memcmp(ptr,output,sizeof output)); // Retired GPU payload immutable.
    assert(waits==0);
    key.pipeline=1;
    assert(cache.store(1,key,spans,2,output,sizeof output));
    assert(!cache.lookup(1,key).resident && cache.stats.residentFallbacks==1);
    assert(cache.cpu_bytes()==cache.bytes());
    array[0]^=1;
    assert(!cache.lookup(1,key).data); // Same address, changed bytes.
    key.pipeline=2;cache.set_output_allocator(alloc,release);
    assert(cache.store(2,key,spans,2,output,sizeof output));
    cache.set_budget(0);assert(cache.bytes()==0 && cache.stats.residentBytes==0);
    assert(cache.cpu_bytes()==0);
    assert(arena.stats.retiredBytes==512);
    completed=false;
    assert(!arena.allocate(sizeof memory)); // Completion failure cannot recycle.
    assert(arena.stats.retiredBytes==512);
    completed=true;
    auto full=arena.allocate(sizeof memory);assert(full==memory);
    assert(arena.stats.peakBytes<=sizeof memory);
    arena.release(full);assert(arena.stats.retiredBytes==sizeof memory);
    assert(!arena.allocate(sizeof memory+1));
    assert(arena.shutdown());
    assert(arena.stats.liveBytes==0 && arena.stats.retiredBytes==0);
    assert(arena.init(memory,finish));
    { GeometryCache c; c.set_output_allocator(alloc,release);
      assert(c.store(0,key,spans,2,output,sizeof output));c.clear();
      assert(c.stats.residentReleases==1); }
    assert(arena.shutdown());
    // Normal append path scans exactly the requested units, not the filled prefix.
    assert(arena.init(memory,finish));
    const auto scans=arena.stats.scannedUnits;
    void* small[1024];
    for(auto& p:small){p=arena.allocate(256);assert(p);}
    assert(arena.stats.scannedUnits-scans==1024);
    const auto atCap=arena.stats.scannedUnits;
    assert(!arena.allocate(256));assert(arena.stats.scannedUnits==atCap);
    for(auto p:small)arena.release(p);
    assert(arena.collect());
    // Full arena forces hint wrap; fragmented free space must not join ends.
    auto middle=arena.allocate(4*1024*1024);assert(middle);
    auto tail=arena.allocate(3*1024*1024);assert(tail);
    arena.release(middle);assert(arena.collect());
    auto wrap=arena.allocate(2*1024*1024);assert(wrap);
    assert(wrap==memory); // Hint near end, physical prefix is the fitting range.
    arena.release(wrap);arena.release(tail);assert(arena.collect());
    // A free run straddling the next-fit hint must still be found.
    auto all=arena.allocate(sizeof memory);assert(all==memory);
    arena.release(all);assert(arena.collect());
    assert(arena.shutdown());
}
