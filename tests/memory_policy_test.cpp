#include "../platform/vita/gfx_vgl/memory_policy.hpp"
#include <cassert>
#include <limits>
#include <initializer_list>
int main() {
 using namespace twv::vita;
 MemoryPolicy p;
 for(uint32_t id=0;id<3;++id) for(uint64_t reserve: {uint64_t(0), ResidentReserveBytes}) {
  assert(memory_policy(id,reserve,p));
  assert(p.total_bytes==128*MemoryMiB);
  assert(p.ram_bytes+reserve==(32+16*id)*MemoryMiB);
  assert(p.scratch_bytes==48*MemoryMiB && p.newlib_bytes==160*MemoryMiB);
 }
 assert(!memory_policy(3,0,p) && p.total_bytes==0);
 assert(!memory_policy(0,1,p));
 assert(!memory_policy(0,33*MemoryMiB,p));
 assert(!memory_policy(0,std::numeric_limits<uint64_t>::max(),p));
 assert(memory_policy(0,32*MemoryMiB,p) && p.ram_bytes==0);
}

