// Test the real builder, decoder and cache; only platform allocation/state APIs
// are recorded. Constant hashes deliberately exercise exact-content collisions.
#define TWV_VITA_GEOMETRY_CACHE 0
#define TWV_VITA_VERTEX_CULL 0
#define TWV_VITA_SPARSE_SPANS 0
#define TWV_VITA_CPU_VERTEX_STAGING 1
#define TWV_VITA_SMALL_PACKET_CACHE 1
#define TWV_VITA_UNCACHED_STAGING 1
#ifndef TWV_VITA_CACHE_VALIDATE
#define TWV_VITA_CACHE_VALIDATE 1
#endif
#define TWV_VITA_STREAM_DECODE 1
#define TWV_VITA_PLANNED_FETCH 1
#define TWV_VITA_FAST_FIXED_SCALE 1
#define TWV_VITA_ATTR_DISPATCH 1
#define TWV_VITA_CANONICAL_GEOMETRY_KEYS 0
#include "gfx_vgl/attribute_stream.hpp"
#include "gfx_vgl/vertex_contract.hpp"
#include "gfx_vgl/small_packet_policy.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include "gfx_vgl/vertex_staging.hpp"
#include "gfx_vgl/staging_policy.hpp"
#include "gfx_vgl/vertex_work_probe.hpp"
#include "gfx_vgl/vertex_decoder_control.hpp"
#include "gfx_vgl/bundle_options.h"
#include "gfx_vgl/counter_batch.hpp"
#include <array>
#include <bitset>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <optional>
namespace aurora::gx {
constexpr unsigned MaxPnMtx=10;
#include "small_packet_types.inc"
struct ShaderConfig {
 uint8_t vtxStride=0;std::array<AttrConfig,GX_VA_TEX7+1> attrs{};
 std::array<ColorChannelConfig,4> colorChannels{};std::array<TcgConfig,8> tcgs{};
};
struct DrawData {
 uint32_t pipeline=1,vtxCount=4;
 struct {uintptr_t vtxStart=0;std::array<uint32_t,12> arrayStart{};uint32_t currentPnMtx=9;} immediateData;
};
}
namespace twv::vita {static unsigned errors=0;void telemetryEvent(const char*,uint64_t){++errors;}}
namespace twv::vgl {
struct Pipeline {
 struct {aurora::gx::ShaderConfig shaderConfig;} config;
 struct {std::bitset<8> sampledTexCoords;} info;
 TwvVitaVertexLayout vertexLayout{};
 bool gpuTexgen=true,usesCurrentPn=true;uint32_t sourceArrayMask=0;
};
using aurora::gx::AttrConfig;
template<class T>void count(T,uint32_t=1){}
static std::vector<std::vector<unsigned char>> ring;
static bool failNextAllocation=false;
static uint32_t memoryClass=0;
static unsigned classQueries=0;
static void* twv_vgl_alloc(size_t bytes){if(failNextAllocation){failNextAllocation=false;return nullptr;}ring.emplace_back(bytes);return ring.back().data();}
static uint32_t twv_vgl_vertex_write_class(const void*ptr,size_t bytes){
 ++classQueries;assert(!ring.empty()&&ptr==ring.back().data()&&bytes==ring.back().size());return memoryClass;
}
static uint32_t twv_vgl_vertex_is_vram(const void*){return memoryClass==uint32_t(VertexWriteClass::Vram);}
static uint64_t twv_vgl_time_us(){return 0;}
static int twv_vita_bundle_feature(uint32_t){return 0;}
static int twv_vgl_resident_available(){return 0;}
static void* twv_vgl_resident_alloc(size_t){std::abort();}
static void twv_vgl_resident_release(void*){std::abort();}
// Bundled xxHash header is absent here. Constant output intentionally stresses
// exact source/key checks; this test makes no assertion about hash distribution.
static uint32_t XXH32(const void*,size_t,uint32_t){return 0;}
// T3 static geometry is off in these builder tests: no certificate, no verify mode.
static StaticCert static_packet_cert(const Pipeline&,const aurora::gx::DrawData&,size_t){return {};}
static bool static_verify(){return false;}
#include "small_packet_vertex_stage.inc"
#include "small_packet_build.inc"
}
using namespace twv::vgl;
static void be32(uint8_t*p,uint32_t value){for(unsigned b=0;b<4;++b)p[3-b]=uint8_t(value>>(8*b));}
static Pipeline pipeline(bool rawST=true,bool directPN=false){
 Pipeline p;auto& cfg=p.config.shaderConfig;
 for(auto& channel:cfg.colorChannels){channel.matSrc=GX_SRC_REG;channel.ambSrc=GX_SRC_REG;}
 auto& pos=cfg.attrs[GX_VA_POS];pos.attrType=GX_DIRECT;pos.compType=GX_F32;pos.cnt=3;pos.offset=0;
 auto& tex=cfg.attrs[GX_VA_TEX0];tex.attrType=GX_DIRECT;tex.compType=GX_F32;tex.cnt=2;tex.offset=12;
 cfg.tcgs[0].src=GX_TG_TEX0;cfg.tcgs[0].type=GX_TG_MTX2x4;p.info.sampledTexCoords.set(0);
 cfg.vtxStride=20;
 if(directPN){cfg.attrs[GX_VA_PNMTXIDX].attrType=GX_DIRECT;cfg.attrs[GX_VA_PNMTXIDX].offset=20;cfg.vtxStride=21;}
 p.vertexLayout=compact_vertex_layout(1,0,false,rawST);return p;
}
static std::vector<uint8_t> packet(const Pipeline&p,unsigned count=4){
 std::vector<uint8_t> source(count*p.config.shaderConfig.vtxStride);
 for(unsigned v=0;v<count;++v){auto*b=source.data()+v*p.config.shaderConfig.vtxStride;
  be32(b,0x3f800000+v);be32(b+4,0x40000000);be32(b+8,0xbf800000);
  be32(b+12,v?0x80000000:0x7fa12345);be32(b+16,0x3f000000);
  if(p.config.shaderConfig.vtxStride==21)b[20]=27;
 }return source;
}
static void reset(bool stream=false){
 geometry_cache_clear();ring.clear();smallPacketCache=SmallPacketCache{};
 uncachedStagingControl=VertexDecoderControl{true}; // Still OFF until explicitly selected.
 uncachedStagedBytes=vramStagedBytes=0;memoryClass=0;classQueries=0;
 smallPacketControl=VertexDecoderControl{true};assert(smallPacketControl.select(1));
 streamDecodeControl=VertexDecoderControl{true};assert(streamDecodeControl.select(stream));
 smallValidationSerial=smallChecks=smallFailures=smallEligible=smallReplayBytes=0;
 failNextAllocation=false;twv::vita::errors=0;
}
static unsigned char* build(const Pipeline&p,aurora::gx::DrawData&d,unsigned&slots){bool culled=true;auto*out=static_cast<unsigned char*>(build_gpu_vertices(p,d,slots,culled));assert(!culled);return out;}
static void oracle(const Pipeline&p,const aurora::gx::DrawData&d,const void*out,unsigned slots){assert(out);VertexStage stage(p,d);assert(stage.validate_raw_streams(p.vertexLayout,static_cast<const uint8_t*>(out),slots));}
int main(){
 // Diagnostic selection remains explicit: OFF decodes normally with no lookup.
 reset();smallPacketControl=VertexDecoderControl{true};assert(smallPacketControl.select(0));
 {auto p=pipeline();auto source=packet(p);aurora::gx::DrawData d;d.immediateData.vtxStart=reinterpret_cast<uintptr_t>(source.data());unsigned slots=0;
  auto*out=build(p,d,slots);oracle(p,d,out,slots);assert(!smallPacketCache.stats.lookupCalls);}
 for(bool rawST:{false,true})for(bool stream:{false,true}){
  reset(stream);auto p=pipeline(rawST);auto source=packet(p);aurora::gx::DrawData d;d.immediateData.vtxStart=reinterpret_cast<uintptr_t>(source.data());
  unsigned slots=0;auto*first=build(p,d,slots);oracle(p,d,first,slots);assert(slots==10&&smallPacketCache.stats.misses==1);
  const auto bytes=size_t(d.vtxCount)*p.vertexLayout.stride;std::vector<uint8_t> original(first,first+bytes);
  auto*hit=build(p,d,slots);assert(hit!=first&&smallPacketCache.stats.hits==1&&smallReplayBytes==bytes);oracle(p,d,hit,slots);
  // Content identity permits relocation, while a same-address mutation misses.
  auto relocated=source;d.immediateData.vtxStart=reinterpret_cast<uintptr_t>(relocated.data());
  const auto hits=smallPacketCache.stats.hits;hit=build(p,d,slots);assert(smallPacketCache.stats.hits==hits+1);oracle(p,d,hit,slots);
  be32(relocated.data(),0x40400000);const auto misses=smallPacketCache.stats.misses;hit=build(p,d,slots);
  assert(smallPacketCache.stats.misses==misses+1&&std::memcmp(hit,original.data(),bytes));oracle(p,d,hit,slots);
  // Actual production cadence: the 128th hit invokes independent decoder oracle.
  smallValidationSerial=0;smallChecks=0;for(unsigned i=1;i<=128;++i){hit=build(p,d,slots);assert(smallChecks==(i==128?1u:0u));}oracle(p,d,hit,slots);
  // Returned hits are mapped-ring copies; damaging one never damages CPU cache.
  hit[0]^=1;auto*again=build(p,d,slots);oracle(p,d,again,slots);
  failNextAllocation=true;assert(!build(p,d,slots));assert(smallPacketControl.enabled());again=build(p,d,slots);oracle(p,d,again,slots);
  geometry_cache_clear();const auto priorMisses=smallPacketCache.stats.misses;again=build(p,d,slots);assert(smallPacketCache.stats.misses==priorMisses+1);oracle(p,d,again,slots);
  // Pipeline IDs key live programs. A reused ID requires production lifecycle clear.
  ++d.pipeline;const auto keyedMiss=smallPacketCache.stats.misses;again=build(p,d,slots);assert(smallPacketCache.stats.misses==keyedMiss+1);
  geometry_cache_clear();p.config.shaderConfig.attrs[GX_VA_TEX0].cnt=1;again=build(p,d,slots);oracle(p,d,again,slots);
 }
 reset();auto p=pipeline(true,true);auto source=packet(p);aurora::gx::DrawData d;d.immediateData.vtxStart=reinterpret_cast<uintptr_t>(source.data());d.immediateData.currentPnMtx=10;
 unsigned slots=0;auto*out=build(p,d,slots);oracle(p,d,out,slots);assert(slots==10);
 // Direct PN9 is independent of invalid implicit PN10; direct PN10 must reject.
 source[20]=30;assert(!build(p,d,slots));source[20]=27;
 reset();p=pipeline();source=packet(p);d.immediateData.vtxStart=reinterpret_cast<uintptr_t>(source.data());d.immediateData.currentPnMtx=10;assert(!build(p,d,slots));d.immediateData.currentPnMtx=9;
 failNextAllocation=true;assert(!build(p,d,slots));assert(!smallPacketCache.stats.hits);out=build(p,d,slots);oracle(p,d,out,slots);
 // Even an UNUSED indexed array prevents small-cache admission. The real stage
 // ignores this unused field, so no fake indexed source is dereferenced.
 for(unsigned type:{GX_INDEX8,GX_INDEX16}){
  geometry_cache_clear();p.config.shaderConfig.attrs[GX_VA_TEX7].attrType=type;
  const auto calls=smallPacketCache.stats.lookupCalls;out=build(p,d,slots);oracle(p,d,out,slots);assert(smallPacketCache.stats.lookupCalls==calls);
 }p.config.shaderConfig.attrs[GX_VA_TEX7].attrType=GX_NONE;
 reset(true);out=build(p,d,slots);oracle(p,d,out,slots);
 SmallPacketKey key{d.pipeline,d.vtxCount,p.config.shaderConfig.vtxStride,d.immediateData.currentPnMtx};
 auto borrowed=smallPacketCache.lookup(0,key,source.data(),source.size(),size_t(d.vtxCount)*p.vertexLayout.stride);assert(borrowed.data);
 const_cast<uint8_t*>(borrowed.data)[0]^=0x40;smallValidationSerial=127;
 out=build(p,d,slots);oracle(p,d,out,slots);
 assert(smallFailures==1&&!smallPacketControl.enabled()&&!streamDecodeControl.enabled()&&twv::vita::errors==1);
 assert(!smallPacketCache.lookup(0,key,source.data(),source.size(),size_t(d.vtxCount)*p.vertexLayout.stride).data);
 const auto lookups=smallPacketCache.stats.lookupCalls;out=build(p,d,slots);oracle(p,d,out,slots);assert(smallPacketCache.stats.lookupCalls==lookups);
 // Real staging path:16 direct vertices,8 texgens aliasing TEX0; source320B,
 // output1280B ST2 or1792B STQ. New uncached selection stays OFF by default.
 for(bool rawST:{false,true})for(unsigned classification:{0u,1u,2u,3u})for(bool selected:{false,true}){
  reset(true);memoryClass=classification;assert(uncachedStagingControl.select(selected));
  auto large=pipeline(rawST);
  for(unsigned tc=0;tc<8;++tc){large.info.sampledTexCoords.set(tc);large.config.shaderConfig.tcgs[tc].src=GX_TG_TEX0;large.config.shaderConfig.tcgs[tc].type=GX_TG_MTX2x4;}
  large.vertexLayout=compact_vertex_layout(255,0,false,rawST);
  auto largeSource=packet(large,16);aurora::gx::DrawData draw;draw.vtxCount=16;draw.immediateData.vtxStart=reinterpret_cast<uintptr_t>(largeSource.data());
  const size_t bytes=size_t(draw.vtxCount)*large.vertexLayout.stride;
  assert(bytes>=1024&&bytes<=SmallPacketCache::OutputLimit&&largeSource.size()<=SmallPacketCache::SourceLimit);
  const bool staged=classification==1 || (selected&&classification==2);
  unsigned extent=0;auto*mapped=build(large,draw,extent);oracle(large,draw,mapped,extent);assert(mapped==ring.back().data());
  assert((vertexStaging.bytes()!=0)==staged);
  assert(uncachedStagedBytes==(staged&&classification==2?bytes:0));assert(vramStagedBytes==(staged&&classification==1?bytes:0));
  assert(classQueries==(selected?1u:0u));
  if(staged){auto*cpu=vertexStaging.acquire(bytes);assert(cpu!=mapped&&!std::memcmp(cpu,mapped,bytes));}
  const auto checks=smallChecks;smallValidationSerial=0;
  for(unsigned i=0;i<127;++i)mapped=build(large,draw,extent);
  const auto streamBefore=streamPackets,streamSerialBefore=streamValidationSerial;
  mapped=build(large,draw,extent);oracle(large,draw,mapped,extent);
  assert(smallChecks==checks+1&&streamPackets==streamBefore&&streamValidationSerial==streamSerialBefore);
  assert(uncachedStagedBytes==(staged&&classification==2?2*bytes:0));assert(vramStagedBytes==(staged&&classification==1?2*bytes:0));
  if(staged){auto*cpu=vertexStaging.acquire(bytes);assert(cpu!=mapped&&!std::memcmp(cpu,mapped,bytes));}
 }
 // Corrupt PN extent alone through the public store API: bytes stay correct.
 reset(true);p=pipeline();source=packet(p);d={};d.immediateData.vtxStart=reinterpret_cast<uintptr_t>(source.data());
 out=build(p,d,slots);oracle(p,d,out,slots);assert(slots==10);
 key={d.pipeline,d.vtxCount,p.config.shaderConfig.vtxStride,d.immediateData.currentPnMtx};
 borrowed=smallPacketCache.lookup(0,key,source.data(),source.size(),size_t(d.vtxCount)*p.vertexLayout.stride);assert(borrowed.data);
 assert(smallPacketCache.store(0,key,source.data(),source.size(),borrowed.bytes,borrowed.data,1));
 smallValidationSerial=127;const auto streamBefore=streamPackets;
 out=build(p,d,slots);oracle(p,d,out,slots);
 assert(slots==10&&smallFailures==1&&!smallPacketControl.enabled()&&!streamDecodeControl.enabled()&&streamPackets==streamBefore);
 assert(!smallPacketCache.lookup(0,key,source.data(),source.size(),size_t(d.vtxCount)*p.vertexLayout.stride).data);
 std::puts("small_packet_integration_test: real builder/VertexStage/cache, replay/original oracle cadence, mutation/relocation/PN, guards and fail-closed corruption passed");
}
