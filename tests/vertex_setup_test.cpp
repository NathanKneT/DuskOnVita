// Real V59/current build_gpu_vertices + VertexStage, not replacement decoders.
#define TWV_VITA_GEOMETRY_CACHE 1
#define TWV_VITA_VERTEX_CULL 0
#define TWV_VITA_SPARSE_SPANS 1
#define TWV_VITA_CPU_VERTEX_STAGING 1
#define TWV_VITA_SMALL_PACKET_CACHE 1
#define TWV_VITA_UNCACHED_STAGING 1
#define TWV_VITA_CACHE_VALIDATE 1
#ifndef TWV_VITA_LAZY_VERTEX_PLAN
#define TWV_VITA_LAZY_VERTEX_PLAN 0
#endif
#ifndef TWV_VITA_STREAM_DECODE
#define TWV_VITA_STREAM_DECODE 1
#endif
#ifndef TWV_VITA_PLANNED_FETCH
#define TWV_VITA_PLANNED_FETCH 1
#endif
#define TWV_VITA_FAST_FIXED_SCALE 1
#define TWV_VITA_ATTR_DISPATCH 1
#define TWV_VITA_CANONICAL_GEOMETRY_KEYS 1
#include "gfx_vgl/attribute_stream.hpp"
#include "gfx_vgl/vertex_contract.hpp"
#include "gfx_vgl/small_packet_policy.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include "gfx_vgl/vertex_staging.hpp"
#include "gfx_vgl/staging_policy.hpp"
#include "gfx_vgl/vertex_work_probe.hpp"
#include "gfx_vgl/vertex_decoder_control.hpp"
#include "gfx_vgl/counter_batch.hpp"
#include "gfx_vgl/bundle_options.h"
#include <aurora/math.hpp>
#include <dolphin/gx.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <sys/mman.h>
#include "vertex_setup_types.inc"

// Production's immediate address words remain real 32-bit words. Use low host
// mappings so the fixture need not widen/reimplement DrawImmediateData.
struct LowBuffer {
 uint8_t* data;size_t size;
 explicit LowBuffer(size_t bytes):size((bytes+4095)&~size_t(4095)) {
  data=static_cast<uint8_t*>(mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0));
  assert(data!=MAP_FAILED && uintptr_t(data)+size<=UINT32_MAX);std::memset(data,0,size);
 }
 ~LowBuffer(){munmap(data,size);}
 LowBuffer(const LowBuffer&)=delete;
};
namespace recording {
std::vector<std::vector<uint8_t>> allocations;
std::map<std::string,uint64_t> counters;
std::vector<std::pair<std::string,uint64_t>> telemetry;
bool failAllocation=false;
unsigned memoryClass=0,allocationCalls=0;
void begin(bool fail=false) {allocations.clear();counters.clear();telemetry.clear();allocationCalls=0;failAllocation=fail;}
}
namespace twv::vita {void telemetryEvent(const char* name,uint64_t value){recording::telemetry.emplace_back(name,value);}}
namespace twv::vgl {
using aurora::gx::AttrConfig;
void count(const char* name,uint32_t amount=1){recording::counters[name]+=amount;}
void count(CounterId id,uint32_t amount=1){count(CounterNames[size_t(id)],amount);}
void* twv_vgl_alloc(size_t bytes){++recording::allocationCalls;if(recording::failAllocation)return nullptr;recording::allocations.emplace_back(std::max(bytes,size_t(1)),0xa5);return recording::allocations.back().data();}
uint32_t twv_vgl_vertex_write_class(const void*,size_t){return recording::memoryClass;}
uint32_t twv_vgl_vertex_is_vram(const void*){return recording::memoryClass==1;}
uint64_t twv_vgl_time_us(){return 0;}
int twv_vita_bundle_feature(uint32_t){return 0;}
int twv_vgl_resident_available(){return 0;}
void* twv_vgl_resident_alloc(size_t){std::abort();}
void twv_vgl_resident_release(void*){std::abort();}
// Collision pressure is deliberate; neither cache may trust pointer/hash alone.
uint32_t XXH32(const void*,size_t,uint32_t){return 0;}
// T3 static geometry is off in these builder tests: no certificate, no verify mode.
StaticCert static_packet_cert(const Pipeline&,const aurora::gx::DrawData&,size_t){return {};}
bool static_verify(){return false;}
namespace original {
#include "vertex_setup_original.inc"
}
namespace candidate {
#include "vertex_setup_candidate.inc"
}
}
using namespace twv::vgl;

struct Result {
 bool success,culled;unsigned slots,allocationCalls,plans;
 std::vector<uint8_t> bytes;
 std::vector<std::vector<uint8_t>> allocations;
 std::map<std::string,uint64_t> counters;
 std::vector<std::pair<std::string,uint64_t>> telemetry;
};
template<class Build> Result run(const Pipeline& p,const aurora::gx::DrawData& d,unsigned& plans,Build build,bool fail=false) {
 recording::begin(fail);plans=0;unsigned slots=0;bool culled=true;
 const auto* out=static_cast<uint8_t*>(build(p,d,slots,culled));
 Result result{out!=nullptr,culled,slots,recording::allocationCalls,plans,{},recording::allocations,recording::counters,recording::telemetry};
 if(out)result.bytes.assign(out,out+size_t(d.vtxCount)*p.vertexLayout.stride);
 return result;
}
unsigned cases=0,omittedPlans=0;
bool lazyRequested=false;
Result compare(const Pipeline& p,const aurora::gx::DrawData& d,bool fail=false) {
 const auto a=run(p,d,original::planCalls,original::build_gpu_vertices,fail);
 const auto b=run(p,d,candidate::planCalls,candidate::build_gpu_vertices,fail);
 assert(a.success==b.success && a.culled==b.culled && a.slots==b.slots);
 assert(a.bytes==b.bytes && a.allocations==b.allocations && a.counters==b.counters && a.telemetry==b.telemetry && a.allocationCalls==b.allocationCalls);
 assert(b.plans<=a.plans);omittedPlans+=a.plans-b.plans;++cases;
 return b;
}
void reset(bool stream,bool small=false,bool large=false) {
 candidate::lazyVertexPlanControl=VertexDecoderControl{bool(TWV_VITA_LAZY_VERTEX_PLAN)};
 assert(!candidate::twv_vita_lazy_vertex_plan_select(-1) && !candidate::twv_vita_lazy_vertex_plan_select(2));
 assert(candidate::twv_vita_lazy_vertex_plan_select(lazyRequested)==int(!lazyRequested || TWV_VITA_LAZY_VERTEX_PLAN));
#define RESET(ns) \
 ns::geometryCache.clear();ns::geometryCacheEnabled=large;ns::smallPacketCache.clear();ns::vertexStaging.clear(); \
 ns::residentAllocatorConfigured=false;ns::streamDecodeControl=VertexDecoderControl{bool(TWV_VITA_STREAM_DECODE)};assert(ns::streamDecodeControl.select(stream)==int(!stream || TWV_VITA_STREAM_DECODE)); \
 ns::smallPacketControl=VertexDecoderControl{true};assert(ns::smallPacketControl.select(small)); \
 ns::sparseSpansControl=VertexDecoderControl{true};assert(ns::sparseSpansControl.select(1)); \
 ns::uncachedStagingControl=VertexDecoderControl{true};assert(ns::uncachedStagingControl.select(1)); \
 ns::streamValidationSerial=ns::streamValidationPass=ns::streamValidationFailure=0;ns::cacheValidationSerial=0; \
 ns::smallValidationSerial=ns::smallChecks=ns::smallFailures=ns::smallEligible=ns::smallReplayBytes=0;
 RESET(original) RESET(candidate)
#undef RESET
 recording::memoryClass=0;
}
void word(uint8_t* p,uint32_t value,bool little=false) {for(unsigned i=0;i<4;++i)p[little?i:3-i]=uint8_t(value>>(i*8));}
Pipeline pipeline(unsigned index,bool little,bool directPn,bool padded=false) {
 Pipeline p{};p.gpuTexgen=true;auto& cfg=p.config.shaderConfig;
 auto& pos=cfg.attrs[GX_VA_POS];pos.attrType=index;pos.compType=GX_F32;pos.cnt=3;pos.offset=0;pos.stride=16;pos.le=little;
 const unsigned posWidth=index==GX_DIRECT?12:index==GX_INDEX8?1:2;
 auto& tex=cfg.attrs[GX_VA_TEX0];tex.attrType=index;tex.compType=GX_S16;tex.cnt=2;tex.frac=3;tex.offset=posWidth;tex.stride=8;tex.le=little;
 cfg.vtxStride=posWidth+(index==GX_DIRECT?4:index==GX_INDEX8?1:2);
 if(directPn){cfg.attrs[GX_VA_PNMTXIDX].attrType=GX_DIRECT;cfg.attrs[GX_VA_PNMTXIDX].offset=cfg.vtxStride++;}
 cfg.tcgs[0].src=GX_TG_TEX0;cfg.tcgs[0].type=GX_TG_MTX2x4;p.info.sampledTexCoords.set(0);
 p.vertexLayout=padded?legacy_vertex_layout():compact_vertex_layout(1,0,false,true);
 original::configure_gpu_cache_dependencies(p);const auto mask=p.sourceArrayMask;const auto current=p.usesCurrentPn;
 candidate::configure_gpu_cache_dependencies(p);assert(p.sourceArrayMask==mask && p.usesCurrentPn==current);
 return p;
}
void packet(const Pipeline& p,LowBuffer& root,LowBuffer& pos,LowBuffer& tex,unsigned count) {
 const auto& cfg=p.config.shaderConfig;const auto type=cfg.attrs[GX_VA_POS].attrType;
 for(unsigned v=0;v<count;++v) {
  auto* p0=root.data+v*cfg.vtxStride;auto* pp=type==GX_DIRECT?p0:pos.data+v*16;
  auto* tp=type==GX_DIRECT?p0+cfg.attrs[GX_VA_TEX0].offset:tex.data+v*8;
  const bool little=type==GX_DIRECT?false:cfg.attrs[GX_VA_POS].le;
  word(pp,v%3==0?0x7fa12345u:v%3==1?0x80000000u:0x3f800000u,little);word(pp+4,0x40000000,little);word(pp+8,0xbf800000,little);
  tp[0]=0x80;tp[1]=0;tp[2]=1;tp[3]=0xff;
  if(type!=GX_DIRECT){p0[0]=type==GX_INDEX8?v:0;if(type==GX_INDEX16)p0[1]=v;auto* idx=p0+cfg.attrs[GX_VA_TEX0].offset;idx[0]=type==GX_INDEX8?v:0;if(type==GX_INDEX16)idx[1]=v;}
  if(cfg.attrs[GX_VA_PNMTXIDX].attrType==GX_DIRECT)p0[cfg.attrs[GX_VA_PNMTXIDX].offset]=uint8_t((v%10)*3);
 }
}
int main() {
 LowBuffer root(4096),position(4096),texcoord(4096);
 // The real selector defaults OFF, rejects unavailable/invalid ON and locks at
 // dependency setup as well as decoding. Failures cannot be re-enabled.
 candidate::lazyVertexPlanControl=VertexDecoderControl{bool(TWV_VITA_LAZY_VERTEX_PLAN)};
 assert(!candidate::lazyVertexPlanControl.enabled() && !candidate::lazyVertexPlanControl.selected());
 auto initial=pipeline(GX_DIRECT,false,false);(void)initial;
 assert(!candidate::twv_vita_lazy_vertex_plan_select(0) && !candidate::twv_vita_lazy_vertex_plan_select(1));
 candidate::lazyVertexPlanControl=VertexDecoderControl{bool(TWV_VITA_LAZY_VERTEX_PLAN)};
 assert(candidate::twv_vita_lazy_vertex_plan_select(TWV_VITA_LAZY_VERTEX_PLAN));candidate::lazyVertexPlanControl.fail();
 assert(!candidate::twv_vita_lazy_vertex_plan_select(1));
 for(bool lazy:{false,true}) {
 lazyRequested=lazy;
 for(bool streamed:{false,true}) for(unsigned type:{GX_DIRECT,GX_INDEX8,GX_INDEX16}) for(bool little:{false,true}) for(bool pn:{false,true}) for(bool padded:{false,true}) {
  reset(streamed);auto p=pipeline(type,little,pn,padded);packet(p,root,position,texcoord,16);
  aurora::gx::DrawData d{};d.pipeline=7;d.vtxCount=16;d.immediateData.vtxStart=uint32_t(uintptr_t(root.data));d.immediateData.currentPnMtx=9;
  d.immediateData.arrayStart[GX_VA_POS-GX_VA_POS]=uint32_t(uintptr_t(position.data));d.immediateData.arrayStart[GX_VA_TEX0-GX_VA_POS]=uint32_t(uintptr_t(texcoord.data));
  const auto r=compare(p,d);assert(r.success && r.slots==10);
  if(candidate::lazyVertexPlanControl.enabled() && TWV_VITA_PLANNED_FETCH && TWV_VITA_STREAM_DECODE && streamed && !padded)assert(r.plans==0);
  assert(!candidate::twv_vita_lazy_vertex_plan_select(1));
  // Stable addresses remain mutable. Every new decode must observe the change.
  (type==GX_DIRECT?root.data:position.data)[0]^=0x40;const auto changed=compare(p,d);assert(changed.bytes!=r.bytes);
  compare(p,d,true);d.vtxCount=0;assert(compare(p,d).success);
  d.vtxCount=16;
  if(pn)root.data[p.config.shaderConfig.attrs[GX_VA_PNMTXIDX].offset]=30;else d.immediateData.currentPnMtx=10;
  assert(!compare(p,d).success);
 }
 // Real small-cache hit validation uses the original decoder at its existing
 // cadence; deferred plans must not replace or weaken that independent oracle.
 reset(true,true);auto p=pipeline(GX_DIRECT,false,false);packet(p,root,position,texcoord,4);
 aurora::gx::DrawData d{};d.pipeline=3;d.vtxCount=4;d.immediateData.vtxStart=uint32_t(uintptr_t(root.data));d.immediateData.currentPnMtx=9;
 compare(p,d);compare(p,d);original::smallValidationSerial=candidate::smallValidationSerial=127;
 const auto validation=compare(p,d);assert(validation.success);if(candidate::lazyVertexPlanControl.enabled())assert(validation.plans==0);
 SmallPacketKey key{3,4,p.config.shaderConfig.vtxStride,9};
 for(auto* cache:{&original::smallPacketCache,&candidate::smallPacketCache}) {
  auto hit=cache->lookup(0,key,root.data,4*p.config.shaderConfig.vtxStride,4*p.vertexLayout.stride);assert(hit.data);const_cast<uint8_t*>(hit.data)[0]^=1;
 }
 original::smallValidationSerial=candidate::smallValidationSerial=127;
 assert(compare(p,d).success && !original::smallPacketControl.enabled() && !candidate::smallPacketControl.enabled());
 // The independent raw-stream oracle still runs at its original cadence.
 reset(true);p=pipeline(GX_DIRECT,false,true);packet(p,root,position,texcoord,16);
 d={};d.pipeline=8;d.vtxCount=16;d.immediateData.vtxStart=uint32_t(uintptr_t(root.data));
 original::streamValidationSerial=candidate::streamValidationSerial=127;compare(p,d);
 assert(original::streamValidationPass==candidate::streamValidationPass);
 if(TWV_VITA_STREAM_DECODE)assert(candidate::streamValidationPass==1);
 // Large-cache hits retain full mutable source checks and the independent
 // original decoder. Array content at a stable address must invalidate replay.
 reset(true,false,true);p=pipeline(GX_INDEX16,true,false);packet(p,root,position,texcoord,32);
 d={};d.pipeline=9;d.vtxCount=32;d.immediateData.vtxStart=uint32_t(uintptr_t(root.data));d.immediateData.currentPnMtx=9;
 d.immediateData.arrayStart[0]=uint32_t(uintptr_t(position.data));d.immediateData.arrayStart[GX_VA_TEX0-GX_VA_POS]=uint32_t(uintptr_t(texcoord.data));
 const auto large=compare(p,d);compare(p,d);assert(original::geometryCache.stats.hits && candidate::geometryCache.stats.hits);
 original::cacheValidationSerial=candidate::cacheValidationSerial=255;const auto largeValidation=compare(p,d);
 if(candidate::lazyVertexPlanControl.enabled())assert(largeValidation.plans==0);
 const auto oldInvalidations=original::geometryCache.stats.invalidations;
 position.data[0]^=0x40;const auto mutated=compare(p,d);assert(mutated.bytes!=large.bytes);
 assert(original::geometryCache.stats.invalidations==oldInvalidations+1 && candidate::geometryCache.stats.invalidations==original::geometryCache.stats.invalidations);
 // Corrupt cached bytes without changing source, force the original oracle,
 // and require the same current-packet repair and cache disable behavior.
 reset(true,false,true);packet(p,root,position,texcoord,32);compare(p,d);
 GeometryKey largeKey{};largeKey.pipeline=uint32_t(d.pipeline);largeKey.vertexBase=d.immediateData.vtxStart;largeKey.vertexCount=d.vtxCount;largeKey.currentPnMtx=d.immediateData.currentPnMtx;
 std::memcpy(largeKey.arrays,d.immediateData.arrayStart.data(),sizeof largeKey.arrays);
 const uint32_t arrayMask=(1u<<(GX_VA_POS-GX_VA_POS))|(1u<<(GX_VA_TEX0-GX_VA_POS));largeKey=canonical_geometry_key(largeKey,arrayMask,true);
 for(auto* cache:{&original::geometryCache,&candidate::geometryCache}) {auto hit=cache->lookup(0,largeKey);assert(hit.data);const_cast<uint8_t*>(hit.data)[0]^=1;}
 original::cacheValidationSerial=candidate::cacheValidationSerial=255;
 assert(compare(p,d).success && !original::geometryCacheEnabled && !candidate::geometryCacheEnabled);
 // Dependency setup reads neither source bytes nor candidate fetch plans.
 original::planCalls=candidate::planCalls=0;original::configure_gpu_cache_dependencies(p);const auto mask=p.sourceArrayMask;candidate::configure_gpu_cache_dependencies(p);
 assert(p.sourceArrayMask==mask);if(candidate::lazyVertexPlanControl.enabled())assert(candidate::planCalls==0);else assert(candidate::planCalls==original::planCalls);
 if(TWV_VITA_PLANNED_FETCH && candidate::lazyVertexPlanControl.enabled()){assert(original::planCalls>0 && omittedPlans>0);}
 }
 std::printf("vertex_setup_test: lazy=%d planned=%d stream=%d, %u real-builder equivalence cases; %u unused fetch plans omitted; source-bearing bodies unchanged; no timing/GPU claims\n",TWV_VITA_LAZY_VERTEX_PLAN,TWV_VITA_PLANNED_FETCH,TWV_VITA_STREAM_DECODE,cases,omittedPlans);
}
