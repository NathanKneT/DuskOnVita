#include "../platform/vita/gfx_vgl/submit_batch.hpp"
#include <cassert>
#include <vector>
int main() {
 using namespace twv::vita;
 TwvVitaDrawState s{}; s.program=1; s.vertex_layout.stride=12;
 for(unsigned a=0;a<TwvVertexAttributeCount;++a) s.vertex_layout.offsets[a]=TwvVertexAttributeAbsent;
 s.vertex_layout.offsets[0]=0;s.vertex_layout.components[0]=3;
 std::vector<float> v(65536*3);
 SubmitObserver o;
 o.observe(s,v.data(),3,nullptr,0,1);
 o.observe(s,v.data()+9,3,nullptr,0,1);
 assert(o.stats().compatible_pairs==1 && o.stats().array_proposal_pairs==1);
 // Every state byte participates (including PN/TEV/layout/textures/raster).
 for(size_t b=0;b<sizeof s;++b) {
  o.reset();o.observe(s,v.data(),3,nullptr,0);
  auto t=s; reinterpret_cast<unsigned char*>(&t)[b]^=1;
  o.observe(t,v.data(),3,nullptr,0);
  assert(o.stats().compatible_pairs==0);
 }
 o.reset();o.observe(s,v.data(),3,nullptr,0,1);o.barrier();
 o.observe(s,v.data()+9,3,nullptr,0,1);assert(!o.stats().compatible_pairs);
 o.reset();o.observe(s,v.data(),3,nullptr,0,1);
 o.observe(s,v.data()+18,3,nullptr,0,1);assert(!o.stats().array_proposal_pairs);
 uint16_t idx[3]={0,1,2};o.observe(s,v.data(),3,idx,3,1);
 o.observe(s,v.data()+9,3,idx,3,1);assert(!o.stats().array_proposal_pairs);
 o.reset();o.observe(s,v.data(),65536,nullptr,0);assert(o.stats().invalid==1);
 o.observe(s,v.data(),4,nullptr,0);assert(o.stats().invalid==2);
 o.observe(s,v.data(),3,idx,2);assert(o.stats().invalid==3);
 o.observe(s,reinterpret_cast<void*>(UINTPTR_MAX-1),3,nullptr,0);assert(o.stats().invalid==4);
 o.reset(); for(unsigned i=0;i<130;++i)o.observe(s,v.data(),3,nullptr,0);
 assert(o.stats().longest_run==64 && o.stats().cap_breaks==2);
 o.reset(); assert(!o.stats().draws);
}
