// --vita-pipeline-fast=off|on|verify (default off): a cheaper pipeline config fill and lookup, through
// link wraps of backend.cpp's populate_pipeline_config and pipeline_ref<gx::PipelineConfig> (Aurora's
// command processor calls both). Both originals stay untouched and remain the off path.
//  - on: populate_pipeline_config overwrites the config Aurora keeps in place, writing the entries the
//    current GX state uses and resetting only those the previous config used; the original zeroes the
//    whole config, fills it and copies it twice. The lookup hashes and compares the compact key
//    (pipeline_fast.hpp) in place instead of hashing (XXH32) and comparing the whole config; a key
//    seen for the first time goes through the original lookup, which finds or creates the pipeline.
//  - verify: both, plus the originals on every call; any byte or index that differs is counted
//    (pipeline_fast_*_mismatch) and the original result is used.
// --vita-gxm-pipeline-memo=on (GXM only) adds a memo of the last key in front of the index
// (pipeline_fast.hpp LastKeyMemo); with verify, each memo answer is checked against the original.
// Runs on the FIFO's thread, like the functions it wraps.
#include "decode_probe.hpp"
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "internal.hpp"
#include "pipeline_fast.hpp"
#include <bit>

using aurora::gx::PipelineConfig;
extern "C" void __real__ZN6aurora2gx24populate_pipeline_configERNS0_14PipelineConfigE11GXPrimitive8GXVtxFmt(
    PipelineConfig&,GXPrimitive,GXVtxFmt) noexcept;
extern "C" aurora::gfx::PipelineRef __real__ZN6aurora3gfx12pipeline_refINS_2gx14PipelineConfigEEEjRKT_(
    const PipelineConfig&);
// Defined by profile_hooks.cpp when the decode probe is compiled in.
extern "C" void twv_decode_probe_mark(unsigned kind) noexcept __attribute__((weak));
extern "C" bool twv_decode_probe_timing() noexcept __attribute__((weak));
extern "C" uint64_t twv_vgl_time_us(void);

namespace aurora::gx {
namespace {
// Same result as backend.cpp's populate_pipeline_config, written in place over the previous result.
void populate_in_place(PipelineConfig& config,GXPrimitive primitive,GXVtxFmt fmt) noexcept {
  auto& s=config.shaderConfig;
  const auto& vtxFmt=g_gxState.vtxFmts[fmt];
  s.fogType=g_gxState.fog.type;
  s.fogRangeEnabled=g_gxState.fog.rangeEnabled;
  u8 vtxOffset=0;
  for(int i=GX_VA_PNMTXIDX;i<=GX_VA_TEX7;++i) {
    const auto attr=static_cast<GXAttr>(i);
    const auto type=g_gxState.vtxDesc[i];
    auto& mapping=s.attrs[i];
    if(type==GX_NONE) {mapping={};continue;}
    const auto& attrFmt=vtxFmt.attrs[i];
    const auto cnt=comp_cnt_count(attr,attrFmt.cnt);
    const bool nbt3=attr==GX_VA_NRM && attrFmt.cnt==GX_NRM_NBT3;
    mapping=AttrConfig{.attrType=static_cast<u8>(type),.cnt=cnt,.compType=static_cast<u8>(attrFmt.type),
        .offset=vtxOffset,.stride=0,.frac=attrFmt.frac,.le=false,.nbt3=nbt3};
    switch(type) {
    case GX_DIRECT: vtxOffset+=comp_type_size(attr,attrFmt.type)*cnt; break;
    case GX_INDEX8:
      mapping.stride=g_gxState.arrays[i].stride;mapping.le=g_gxState.arrays[i].le;
      vtxOffset+=nbt3?3:1; break;
    case GX_INDEX16:
      mapping.stride=g_gxState.arrays[i].stride;mapping.le=g_gxState.arrays[i].le;
      vtxOffset+=nbt3?6:2; break;
    default: break;
    }
  }
  s.vtxStride=vtxOffset;
  s.lineMode=primitive==GX_LINES?1:primitive==GX_LINESTRIP?2:primitive==GX_POINTS?3:0;
  s.tevSwapTable=g_gxState.tevSwapTable;
  const size_t oldTev=s.tevStageCount;
  for(u8 i=0;i<g_gxState.numTevStages;++i)s.tevStages[i]=g_gxState.tevStages[i];
  twv::vgl::reset_entries(s.tevStages,g_gxState.numTevStages,oldTev);
  s.tevStageCount=g_gxState.numTevStages;
  const size_t oldInd=s.numIndStages;
  for(u8 i=0;i<g_gxState.numIndStages;++i)s.indStages[i]=g_gxState.indStages[i];
  twv::vgl::reset_entries(s.indStages,g_gxState.numIndStages,oldInd);
  s.numIndStages=g_gxState.numIndStages;
  for(u8 i=0;i<MaxColorChannels;++i) {
    const auto& cc=g_gxState.colorChannelConfig[i];
    if(cc.lightingEnabled)s.colorChannels[i]=cc;
    else s.colorChannels[i]={.matSrc=cc.matSrc};
  }
  for(u8 i=0;i<g_gxState.numTexGens;++i)s.tcgs[i]=g_gxState.tcgs[i];
  twv::vgl::reset_entries(s.tcgs,g_gxState.numTexGens,s.tcgs.size());
  if(g_gxState.alphaCompare)s.alphaCompare=g_gxState.alphaCompare;
  else s.alphaCompare={};
  const bool hasAlpha=efb_has_alpha(g_gxState.pixelFmt);
  const bool alphaUpdate=hasAlpha && g_gxState.alphaUpdate;
  const auto blendFactor=[hasAlpha](GXBlendFactor factor) {
    if(!hasAlpha) {
      if(factor==GX_BL_DSTALPHA)return GX_BL_ONE;
      if(factor==GX_BL_INVDSTALPHA)return GX_BL_ZERO;
    }
    return factor;
  };
  config.version=GXPipelineConfigVersion;
  config.msaaSamples=1;
  config.depthFunc=g_gxState.depthFunc;
  config.cullMode=s.lineMode==0?g_gxState.cullMode:GX_CULL_NONE;
  config.blendMode=g_gxState.blendMode;
  config.blendFacSrc=blendFactor(g_gxState.blendFacSrc);
  config.blendFacDst=blendFactor(g_gxState.blendFacDst);
  config.blendOp=g_gxState.blendOp;
  config.dstAlpha=alphaUpdate?g_gxState.dstAlpha:UINT32_MAX;
  config.polygonOffsetBits=0;
  config.polygonOffsetScaleBits=0;
  config.polygonOffsetClampBits=std::bit_cast<uint32_t>(g_gxState.clamp);
  config.depthCompare=g_gxState.depthCompare;
  config.depthUpdate=g_gxState.depthUpdate;
  config.alphaUpdate=alphaUpdate;
  config.colorUpdate=g_gxState.colorUpdate;
}
} // namespace
} // namespace aurora::gx

namespace {
enum Mode : int {Off,On,Verify};
int sMode=Off;
bool sSelected=false,sStarted=false;
const PipelineConfig* sSynced=nullptr; // the config the last populate wrote, in the canonical form
PipelineConfig sCheck{};
twv::vgl::CompactPipelineIndex sIndex;
twv::vgl::LastKeyMemo sMemo; // --vita-gxm-pipeline-memo
struct {uint32_t populates,lookups,hits,misses,timedPopulates,timedLookups,populateUs,lookupUs,
        memoHits,memoChecks;} sPending{};

// Read at the first lookup: the GPU features are chosen at start-up, before any draw.
bool memo_on() noexcept {
  static const bool on=(twv_gpu_features()&TwvGpuFeaturePipelineMemo)!=0;
  return on;
}

void flush_counts() noexcept {
  if(sPending.lookups<64)return;
  twv::vgl::count("pipeline_fast_populates",sPending.populates);
  twv::vgl::count("pipeline_fast_lookups",sPending.lookups);
  twv::vgl::count("pipeline_fast_hits",sPending.hits);
  twv::vgl::count("pipeline_fast_misses",sPending.misses);
  if(memo_on()) {
    twv::vgl::count("pipeline_memo_hits",sPending.memoHits);   // lookups answered by the memo
    twv::vgl::count("pipeline_memo_checks",sPending.memoChecks); // verify: memo answers equal to the original
  }
  // Both calls timed on the decode probe's timed frames, in every mode (the off figures are the originals').
  twv::vgl::count("pipeline_timed_populates",sPending.timedPopulates);
  twv::vgl::count("pipeline_timed_populate_us",sPending.populateUs);
  twv::vgl::count("pipeline_timed_lookups",sPending.timedLookups);
  twv::vgl::count("pipeline_timed_lookup_us",sPending.lookupUs);
  sPending={};
}
void probe_mark() noexcept {
  if(twv_decode_probe_mark)twv_decode_probe_mark(twv::vgl::DecPipeline);
}
bool timed() noexcept {return twv_decode_probe_timing && twv_decode_probe_timing();}
}

// Start-up only (main_vita.cpp), before the first draw: 0 off, 1 on, 2 verify.
extern "C" int twv_vita_pipeline_fast_select(int mode) {
  if(sSelected || sStarted || mode<Off || mode>Verify)return 0;
  if(mode!=Off && !twv::vgl::pipeline_compact_layout_ok<PipelineConfig>())return 0;
  sMode=mode;sSelected=true;
  return 1;
}

namespace {
void populate(PipelineConfig& config,GXPrimitive primitive,GXVtxFmt fmt) noexcept {
  if(sMode==Off || &config!=sSynced) {
    __real__ZN6aurora2gx24populate_pipeline_configERNS0_14PipelineConfigE11GXPrimitive8GXVtxFmt(config,primitive,fmt);
    if(sMode!=Off)sSynced=&config;
    return;
  }
  ++sPending.populates;
  aurora::gx::populate_in_place(config,primitive,fmt);
  if(sMode!=Verify)return;
  __real__ZN6aurora2gx24populate_pipeline_configERNS0_14PipelineConfigE11GXPrimitive8GXVtxFmt(sCheck,primitive,fmt);
  if(std::memcmp(&sCheck,&config,sizeof config)!=0) {
    twv::vgl::count("pipeline_fast_populate_mismatch");
    config=sCheck;
  } else {
    twv::vgl::count("pipeline_fast_populate_checks");
  }
}
aurora::gfx::PipelineRef lookup(const PipelineConfig& config) {
  if(sMode==Off)return __real__ZN6aurora3gfx12pipeline_refINS_2gx14PipelineConfigEEEjRKT_(config);
  const auto key=twv::vgl::pipeline_compact_key(config);
  const bool memo=memo_on();
  const uint32_t remembered=memo?sMemo.find(key):twv::vgl::CompactPipelineIndex::Missing;
  if(sMode!=Verify && remembered!=twv::vgl::CompactPipelineIndex::Missing) {
    ++sPending.hits;++sPending.memoHits;
    return remembered;
  }
  const uint32_t hash=twv::vgl::key_hash(key);
  const uint32_t found=sIndex.find(hash,key);
  if(sMode==Verify) {
    const auto original=__real__ZN6aurora3gfx12pipeline_refINS_2gx14PipelineConfigEEEjRKT_(config);
    if(found==twv::vgl::CompactPipelineIndex::Missing)sIndex.insert(hash,key,original);
    else if(found!=original)twv::vgl::count("pipeline_fast_lookup_mismatch");
    else twv::vgl::count("pipeline_fast_lookup_checks");
    if(remembered!=twv::vgl::CompactPipelineIndex::Missing) {
      if(remembered!=original)twv::vgl::count("pipeline_memo_mismatch");
      else ++sPending.memoChecks;
    }
    if(memo)sMemo.store(key,original);
    return original;
  }
  if(found!=twv::vgl::CompactPipelineIndex::Missing) {
    ++sPending.hits;
    if(memo)sMemo.store(key,found);
    return found;
  }
  ++sPending.misses;
  const auto id=__real__ZN6aurora3gfx12pipeline_refINS_2gx14PipelineConfigEEEjRKT_(config);
  sIndex.insert(hash,key,id);
  if(memo)sMemo.store(key,id);
  return id;
}
}

extern "C" void __wrap__ZN6aurora2gx24populate_pipeline_configERNS0_14PipelineConfigE11GXPrimitive8GXVtxFmt(
    PipelineConfig& config,GXPrimitive primitive,GXVtxFmt fmt) noexcept {
  probe_mark();
  sStarted=true;
  if(!timed()) {populate(config,primitive,fmt);return;}
  const uint64_t start=twv_vgl_time_us();
  populate(config,primitive,fmt);
  sPending.populateUs+=uint32_t(twv_vgl_time_us()-start);
  ++sPending.timedPopulates;
}

extern "C" aurora::gfx::PipelineRef __wrap__ZN6aurora3gfx12pipeline_refINS_2gx14PipelineConfigEEEjRKT_(
    const PipelineConfig& config) {
  probe_mark();
  sStarted=true;
  ++sPending.lookups;
  aurora::gfx::PipelineRef id;
  if(!timed()) {
    id=lookup(config);
  } else {
    const uint64_t start=twv_vgl_time_us();
    id=lookup(config);
    sPending.lookupUs+=uint32_t(twv_vgl_time_us()-start);
    ++sPending.timedLookups;
  }
  flush_counts();
  return id;
}
