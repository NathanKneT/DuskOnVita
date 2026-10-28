// VitaGL backend for the original Aurora GX frontend. No platform/3ds includes.
#include "gx/gx.hpp"
#include "gfx/clear.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/recording.hpp"
#include "gfx/resources.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gfx/texture.hpp"
#include "gfx/texture_replacement.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include "internal.hpp"
#include "io.hpp"
#include "window.hpp"
#include "backend.hpp"
#include "profile_scope.hpp"
#include "cpu_cost_probe.hpp"
#include "gx_fast_api.h"
#include "vertex_contract.hpp"
#include "exact_key_index.hpp"
#include "vertex_decoder_control.hpp"
#include "gfx/hash.hpp"
#include "gx/texture.hpp"
#include "runtime/telemetry.hpp"
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

#include <aurora/texture.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
// Counters of the frame being recorded; published to resources().stats at end of
// frame, so aurora_get_stats() reports the last completed frame as upstream does.
AuroraStats sFrameStats{};
twv::vgl::CpuCostProbe sCpuCostProbe;
struct PipelineInfoReuseStats {
 // Frontend and reuse-oracle builds; backend creation is in CpuCostProbe misses.
 uint64_t requests=0,reused=0,built=0,fallback=0,oracleChecks=0,oracleFailures=0,invalid=0,memoHits=0;
 // --vita-gxm-pipeline-memo (twv_vita_pipeline_info_copy): -1 until read at the first call, then the
 // pipeline last checked there, with copies of its config and info that stay in cache.
 int memoOn=-1;
 uint32_t memoPipeline=UINT32_MAX;
 aurora::gx::PipelineConfig memoConfig{};
 aurora::gx::ShaderInfo memoInfo{};
} sPipelineInfoReuse;
}

namespace aurora::gfx::detail {
Resources& resources() noexcept {
  static Resources resources;
  return resources;
}

void increment_merged_draw_count() noexcept { ++sFrameStats.mergedDrawCallCount; }
} // namespace aurora::gfx::detail

// --- GX view state (lib/gx/gx.cpp, unchanged logic) ---
namespace aurora::gx {
GXState g_gxState{};

namespace {
std::atomic<int> sPendingViewportPolicy{-1};
}

void set_viewport_policy(AuroraViewportPolicy policy) noexcept {
  sPendingViewportPolicy.store(policy, std::memory_order_release);
}

void update() noexcept {
  if (const int pending = sPendingViewportPolicy.exchange(-1, std::memory_order_acq_rel); pending != -1) {
    const auto policy = static_cast<AuroraViewportPolicy>(pending);
    g_gxState.viewportPolicy = policy;
    window::set_frame_buffer_aspect_fit(policy == AURORA_VIEWPORT_FIT);
  }
}

Vec2<uint32_t> logical_fb_size() noexcept {
  return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();
}

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalViewport;
  }
  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalViewport;
  }
  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  return {
      .left = logicalViewport.left * scaleX,
      .top = logicalViewport.top * scaleY,
      .width = logicalViewport.width * scaleX,
      .height = logicalViewport.height * scaleY,
      .znear = logicalViewport.znear,
      .zfar = logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }
  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalScissor;
  }
  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const float left = static_cast<float>(logicalScissor.x) * scaleX;
  const float top = static_cast<float>(logicalScissor.y) * scaleY;
  const float right = static_cast<float>(logicalScissor.x + logicalScissor.width) * scaleX;
  const float bottom = static_cast<float>(logicalScissor.y + logicalScissor.height) * scaleY;
  const auto mappedLeft = std::clamp(static_cast<int32_t>(std::floor(left)), 0, static_cast<int32_t>(targetWidth));
  const auto mappedTop = std::clamp(static_cast<int32_t>(std::floor(top)), 0, static_cast<int32_t>(targetHeight));
  const auto mappedRight =
      std::clamp(static_cast<int32_t>(std::ceil(right)), mappedLeft, static_cast<int32_t>(targetWidth));
  const auto mappedBottom =
      std::clamp(static_cast<int32_t>(std::ceil(bottom)), mappedTop, static_cast<int32_t>(targetHeight));
  return {
      .x = mappedLeft,
      .y = mappedTop,
      .width = mappedRight - mappedLeft,
      .height = mappedBottom - mappedTop,
  };
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.logicalViewport.left || viewport.width != g_gxState.logicalViewport.width ||
      viewport.height != g_gxState.logicalViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.renderViewport.left || viewport.width != g_gxState.renderViewport.width ||
      viewport.height != g_gxState.renderViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.renderViewport = viewport;
  gfx::set_viewport(viewport);
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(g_gxState.logicalScissor));
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.renderScissor = scissor;
  gfx::set_scissor(scissor);
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  // Output object footprint, not a measured memcpy or FIFO byte count.
  twv::vgl::CpuDescriptorScope cpuCost(sCpuCostProbe,sizeof config);
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  config.shaderConfig = {};
  config.shaderConfig.fogType = g_gxState.fog.type;
  config.shaderConfig.fogRangeEnabled = g_gxState.fog.rangeEnabled;
  u8 vtxOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    auto& mapping = config.shaderConfig.attrs[i];
    if (type == GX_NONE) {
      mapping = {};
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    const auto cnt = comp_cnt_count(attr, attrFmt.cnt);
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    mapping = AttrConfig{
        .attrType = static_cast<u8>(type),
        .cnt = cnt,
        .compType = static_cast<u8>(attrFmt.type),
        .offset = vtxOffset,
        .stride = 0,
        .frac = attrFmt.frac,
        .le = false,
        .nbt3 = nbt3,
    };
    switch (type) {
    case GX_DIRECT:
      vtxOffset += comp_type_size(attr, attrFmt.type) * cnt;
      break;
    case GX_INDEX8:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 3 : 1;
      break;
    case GX_INDEX16:
      mapping.stride = g_gxState.arrays[i].stride;
      mapping.le = g_gxState.arrays[i].le;
      vtxOffset += nbt3 ? 6 : 2;
      break;
    default:
      break;
    }
  }
  config.shaderConfig.vtxStride = vtxOffset;
  if (primitive == GX_LINES) {
    config.shaderConfig.lineMode = 1;
  } else if (primitive == GX_LINESTRIP) {
    config.shaderConfig.lineMode = 2;
  } else if (primitive == GX_POINTS) {
    config.shaderConfig.lineMode = 3;
  } else {
    config.shaderConfig.lineMode = 0;
  }
  config.shaderConfig.tevSwapTable = g_gxState.tevSwapTable;
  for (u8 i = 0; i < g_gxState.numTevStages; ++i) {
    config.shaderConfig.tevStages[i] = g_gxState.tevStages[i];
  }
  config.shaderConfig.tevStageCount = g_gxState.numTevStages;
  for (u8 i = 0; i < g_gxState.numIndStages; ++i) {
    config.shaderConfig.indStages[i] = g_gxState.indStages[i];
  }
  config.shaderConfig.numIndStages = g_gxState.numIndStages;
  for (u8 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = g_gxState.colorChannelConfig[i];
    if (cc.lightingEnabled) {
      config.shaderConfig.colorChannels[i] = cc;
    } else {
      config.shaderConfig.colorChannels[i] = {
          .matSrc = cc.matSrc,
      };
    }
  }
  for (u8 i = 0; i < g_gxState.numTexGens; ++i) {
    config.shaderConfig.tcgs[i] = g_gxState.tcgs[i];
  }
  if (g_gxState.alphaCompare) {
    config.shaderConfig.alphaCompare = g_gxState.alphaCompare;
  }
  // GX2 polygon offsets do not apply to the GameCube game: left at zero.
  const auto cullMode = config.shaderConfig.lineMode == 0 ? g_gxState.cullMode : GX_CULL_NONE;
  const bool hasAlpha = efb_has_alpha(g_gxState.pixelFmt);
  const bool alphaUpdate = hasAlpha && g_gxState.alphaUpdate;
  const auto blendFactor = [hasAlpha](GXBlendFactor factor) {
    if (!hasAlpha) {
      if (factor == GX_BL_DSTALPHA) {
        return GX_BL_ONE;
      }
      if (factor == GX_BL_INVDSTALPHA) {
        return GX_BL_ZERO;
      }
    }
    return factor;
  };
  config = {
      .shaderConfig = config.shaderConfig,
      .depthFunc = g_gxState.depthFunc,
      .cullMode = cullMode,
      .blendMode = g_gxState.blendMode,
      .blendFacSrc = blendFactor(g_gxState.blendFacSrc),
      .blendFacDst = blendFactor(g_gxState.blendFacDst),
      .blendOp = g_gxState.blendOp,
      .dstAlpha = alphaUpdate ? g_gxState.dstAlpha : UINT32_MAX,
      .polygonOffsetBits = 0,
      .polygonOffsetScaleBits = 0,
      .polygonOffsetClampBits = std::bit_cast<uint32_t>(g_gxState.clamp),
      .depthCompare = g_gxState.depthCompare,
      .depthUpdate = g_gxState.depthUpdate,
      .alphaUpdate = alphaUpdate,
      .colorUpdate = g_gxState.colorUpdate,
  };
}

// The draw reads the live texture binds (g_gxState.textures); the group value only has to be
// stable for the command processor's caching.
GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  (void)info;
  return {};
}
} // namespace aurora::gx

// Original uniform producer is WebGPU-only; draw reads original live GX state.
extern "C" aurora::gfx::Range __wrap__ZN6aurora2gx13build_uniformERKNS0_10ShaderInfoE(const aurora::gx::ShaderInfo&) { return {.size=1}; }
namespace twv::vgl {
namespace {
std::vector<Pipeline> pipelines;
ExactKeyIndex<aurora::gx::PipelineConfig> pipelineIndex;
std::map<std::string,uint64_t,std::less<>> counters;
#if TWV_VITA_BATCH_COUNTERS
CounterBatch hotCounters;
#endif
bool ready=false, in_frame=false, offscreen=false;
// This capability gates BOTH the actual draw rejection and the CPU shortcut.
// Implementing FBO drawing must change it here; supported passes cannot be skipped.
constexpr bool OffscreenDrawSupported=false;
// EFB copies (resolve_pass_into) are not implemented either. A draw that samples a copy would
// read storage nothing wrote and paint it over the scene: retail water (fbtex_dummy) shows the
// copied scene behind it, and here it came out opaque black (F_SP104 opening, village, Faron).
// Leaving the destination untouched is the closest picture until copies exist; implementing
// them must change this, so that no supported copy is skipped.
constexpr bool CopyResolveSupported=false;
bool samples_unresolved_copy(const Pipeline& p) noexcept {
 const auto& textures=aurora::gx::g_gxState.textures;
 for(int i=0;i<8;++i) if(p.info.sampledTextures.test(i)) {
  const auto* texture=static_cast<const Texture*>(textures[i].ref.get());
  if(texture && texture->efbCopy) return true;
 }
 return false;
}
VertexDecoderControl shadowImageSkipControl{bool(TWV_VITA_SHADOW_IMAGE_SKIP)};
// Counted by the game thread's skip queries, merged at end_frame after the FIFO drain:
// with the GX worker on, hotCounters belong to the worker.
CounterBatch gameThreadCounters;
uint32_t frame_count=0, off_w=0, off_h=0;
// Draws dropped because their off-screen target cannot be drawn, by target size (one size
// per pass: shadow, bloom, depth of field, minimap), so a run shows what each pass costs.
struct OffscreenDiscards {uint32_t w,h,draws;};
std::array<OffscreenDiscards,8> offscreenDiscards{};
void tally_offscreen_discard() noexcept {
 for(auto& t:offscreenDiscards) if(!t.draws || (t.w==off_w && t.h==off_h)) {t.w=off_w;t.h=off_h;++t.draws;return;}
}
unsigned light_sample_batches=0;
// The game lays itself out for the full 960x544 screen (Dusklight widens its logical 2D space and
// camera aspect to the window), so it is drawn full width: a 724-wide box squeezed it to 75%.
aurora::gfx::Viewport viewport{0,0,960,544,0,1};
aurora::gfx::ClipRect scissor{0,0,960,544};
constexpr int GameLeft=0, GameWidth=960, GameHeight=544;
}
void count(const char* name,uint32_t amount) noexcept {
 const auto it=counters.find(name);
 if(it!=counters.end())it->second+=amount;
 else counters.emplace(name,amount);
}
void count(CounterId id,uint32_t amount) noexcept {
 const size_t slot=size_t(id);
 if(slot>=CounterNames.size()) {count("invalid_counter_id",1);return;}
#if TWV_VITA_BATCH_COUNTERS
 hotCounters.add(id,amount);
#else
 count(CounterNames[slot],amount);
#endif
}
void count_game(CounterId id,uint32_t amount) noexcept {gameThreadCounters.add(id,amount);}
void flush_hot_counters() noexcept {
 const auto merge=[](const char* name,uint64_t delta) {
  const auto it=counters.find(name);
  if(it!=counters.end())it->second+=delta;
  else counters.emplace(name,delta);
 };
#if TWV_VITA_BATCH_COUNTERS
 hotCounters.flush(merge);
#endif
 gameThreadCounters.flush(merge);
}
}
namespace twv::gfx_vgl {
// Plain C integer ABI between the SDK entry point and game (different enum ABI).
extern "C" int twv_vita_shadow_image_skip_select(int enabled) {
 return vgl::shadowImageSkipControl.select(enabled);
}
// Game thread. Reads only start-up state, never vgl::offscreen: with the GX worker on, the
// worker may not have processed the pass's BEGIN_OFFSCREEN yet. dDlst_shadowReal_c::imageDraw
// is only called inside the shadow pass's GXCreateFrameBuffer (TARGET_PC), so it is off-screen.
extern "C" int twv_vita_shadow_image_skip() {
 vgl::shadowImageSkipControl.begin();
 vgl::count_game(vgl::CounterId::ShadowImageCalls);
 return vgl::shadowImageSkipControl.enabled() && !vgl::OffscreenDrawSupported;
}
extern "C" void twv_vita_shadow_image_skipped(unsigned models,unsigned shapes) {
 vgl::count_game(vgl::CounterId::ShadowImageSkippedCalls);
 vgl::count_game(vgl::CounterId::ShadowImageSkippedModels,models);
 vgl::count_game(vgl::CounterId::ShadowImageRestoredPackets,shapes);
}
void init() noexcept {
 vgl::ready=twv_vgl_init()!=0;
 if(!vgl::ready) { twv::vita::telemetryText("gfx_init_failure","vitaGL initialization failed"); std::abort(); }
}
bool begin_frame() noexcept {
 if(!vgl::ready) return false;
 twv_vgl_begin();
 // recording.cpp::begin_recording starts each original EFB pass with these
 // clear values. Native GL uses forward depth; GXState stores retail 24-bit Z.
 const auto& state=aurora::gx::g_gxState;
 twv_vgl_clear(state.clearColor.x(),state.clearColor.y(),state.clearColor.z(),state.clearColor.w(),
  float(state.clearDepth)/16777215.f,7,vgl::GameLeft,0,vgl::GameWidth,vgl::GameHeight);
 vgl::in_frame=true; return true;
}
void end_frame() noexcept {
 if(vgl::in_frame) twv_vgl_end();
 vgl::in_frame=false;
 for(auto& array:aurora::gx::g_gxState.arrays) array.cachedRange={};
 aurora::gx::texture::end_frame();
 aurora::gfx::detail::resources().stats=sFrameStats; sFrameStats={};
 vgl::geometry_cache_end_frame();
 if(++vgl::frame_count%60==0) {
  twv::vita::telemetryEvent("gfx_shadow_image_skip_compiled",vgl::shadowImageSkipControl.compiled());
  twv::vita::telemetryEvent("gfx_shadow_image_skip_explicit",vgl::shadowImageSkipControl.selected());
  twv::vita::telemetryEvent("gfx_shadow_image_skip_enabled",vgl::shadowImageSkipControl.enabled());
  twv::vita::telemetryEvent("gfx_offscreen_draw_supported",vgl::OffscreenDrawSupported);
  vgl::geometry_cache_report();
  sCpuCostProbe.report(vgl::frame_count,[](const char* key,uint64_t value) {
   twv::vita::telemetryEvent(key,value);
  });
  twv_vita_gx_fast_report();
  twv::vita::telemetryEvent("gfx_shader_info_reused",sPipelineInfoReuse.reused);
  twv::vita::telemetryEvent("gfx_shader_info_built",sPipelineInfoReuse.built);
  twv::vita::telemetryEvent("gfx_shader_info_fallback",sPipelineInfoReuse.fallback);
  twv::vita::telemetryEvent("gfx_shader_info_oracle_checks",sPipelineInfoReuse.oracleChecks);
  twv::vita::telemetryEvent("gfx_shader_info_oracle_failures",sPipelineInfoReuse.oracleFailures);
  twv::vita::telemetryEvent("gfx_shader_info_invalid",sPipelineInfoReuse.invalid);
  if(twv_gpu_features()&TwvGpuFeaturePipelineMemo)
   twv::vita::telemetryEvent("gfx_shader_info_memo_hits",sPipelineInfoReuse.memoHits);
  vgl::flush_hot_counters();
  for(const auto& [key,value]:vgl::counters) twv::vita::telemetryEvent(("gfx_"+key).c_str(),value);
  for(const auto& t:vgl::offscreenDiscards) if(t.draws)
   twv::vita::telemetryEvent(("gfx_offscreen_discard_"+std::to_string(t.w)+"x"+std::to_string(t.h)).c_str(),t.draws);
  twv::vita::telemetryEvent("game_frames",vgl::frame_count);
  twv::vita::telemetryFlush();
 }
}
void shutdown() noexcept {
 vgl::geometry_cache_clear();
 for(auto& pipeline:vgl::pipelines)for(auto program:pipeline.programs)twv_vgl_program_delete(program);
 vgl::pipelines.clear();
 vgl::pipelineIndex.clear();
 sPipelineInfoReuse.memoPipeline=UINT32_MAX; // the memo's pipeline is gone
 twv_vgl_shutdown();
}
}
namespace aurora::gx { void shutdown() noexcept { texture::shutdown(); } }

// Game/int-enum ABI only: these types never cross gpu_api.h into the SDK TU.
// No pointer into pipelines survives this call (growth and shutdown invalidate it).
namespace {
bool same_shader_info(const aurora::gx::ShaderInfo& a,const aurora::gx::ShaderInfo& b) noexcept {
 return a.sampledTexCoords==b.sampledTexCoords && a.sampledTextures==b.sampledTextures &&
  a.sampledKColors==b.sampledKColors && a.sampledColorChannels==b.sampledColorChannels &&
  a.loadsTevReg==b.loadsTevReg && a.writesTevReg==b.writesTevReg && a.usesPTTexMtx==b.usesPTTexMtx &&
  a.indexAttr==b.indexAttr && a.usedIndStages==b.usedIndStages && a.sampledIndTextures==b.sampledIndTextures &&
  a.usedIndTexMtxs==b.usedIndTexMtxs && a.uniformSize==b.uniformSize && a.usesFog==b.usesFog &&
  a.lightingEnabled==b.lightingEnabled && a.lineMode==b.lineMode;
}
[[noreturn]] void pipeline_info_failure(unsigned reason) noexcept {
 twv_vita_gx_fast_disable(reason);
 // A frontend-only fallback cannot repair cached backend metadata/derived layout.
 twv::vita::telemetryEvent("gfx_shader_info_failure_reason",reason);
 twv::vita::telemetryFlush();
 std::abort();
}
}
extern "C" void twv_vita_pipeline_info_built(int fallback) noexcept {
 ++sPipelineInfoReuse.built;
 if(fallback)++sPipelineInfoReuse.fallback;
}
extern "C" int twv_vita_pipeline_info_copy(uint32_t pipeline,const aurora::gx::PipelineConfig* expected,
                                         aurora::gx::ShaderInfo* out) noexcept {
 if(!expected || !out)return 0;
 // --vita-gxm-pipeline-memo: a pipeline's stored config never changes (pipelines are cleared only at
 // shutdown), so a config equal to the copy of the one last checked for that pipeline passes the
 // check against the stored one: the whole-config comparison runs against the hot copy instead of
 // the pipeline's own, rarely cached, config.
 auto& memo=sPipelineInfoReuse;
 if(memo.memoOn<0)memo.memoOn=(twv_gpu_features()&TwvGpuFeaturePipelineMemo)!=0;
 const bool remembered=memo.memoOn && pipeline==memo.memoPipeline &&
  std::memcmp(&memo.memoConfig,expected,sizeof *expected)==0;
 if(!remembered) {
  if(pipeline>=twv::vgl::pipelines.size()) {
   ++sPipelineInfoReuse.invalid;pipeline_info_failure(3);
  }
  if(std::memcmp(&twv::vgl::pipelines[pipeline].config,expected,sizeof *expected)!=0) {
   ++sPipelineInfoReuse.invalid;pipeline_info_failure(3);
  }
 }
 const auto& cached=twv::vgl::pipelines[pipeline];
 const auto serial=++sPipelineInfoReuse.requests;
 // Always available, independently of TWV_VITA_CACHE_VALIDATE. This reference
 // uses the current draw's config, not the cached pipeline's own config.
 if(serial==1 || serial%128==0) {
  ++sPipelineInfoReuse.oracleChecks;++sPipelineInfoReuse.built;
  const auto original=aurora::gx::build_shader_info(expected->shaderConfig);
  if(!same_shader_info(cached.info,original)) {
   ++sPipelineInfoReuse.oracleFailures;pipeline_info_failure(2);
  }
 }
 if(remembered) {*out=memo.memoInfo;++sPipelineInfoReuse.memoHits;++sPipelineInfoReuse.reused;return 1;}
 *out=cached.info;
 if(memo.memoOn) {memo.memoPipeline=pipeline;memo.memoConfig=*expected;memo.memoInfo=cached.info;}
 ++sPipelineInfoReuse.reused;return 1;
}
namespace aurora::gfx {
uint32_t align_uniform(uint32_t n) { return (n+255u)&~255u; }
bool uses_reversed_z() noexcept { return false; }
Range push_verts(const uint8_t* data,size_t size,size_t alignment) {
 (void)alignment; sFrameStats.lastVertSize+=size;
 return {.offset=uint32_t(reinterpret_cast<uintptr_t>(data)),.size=uint32_t(size)};
}
Range push_storage(const uint8_t* data,size_t size) {
 sFrameStats.lastStorageSize+=size;
 return {.offset=uint32_t(reinterpret_cast<uintptr_t>(data)),.size=uint32_t(size)};
}
Range push_indices(const uint8_t* data,size_t size,size_t alignment) {
 (void)alignment; sFrameStats.lastIndexSize+=size;
 // G2 (--vita-gxm-resident): indices inside the static list being processed stay in GPU memory.
 if(const void* resident=twv::vgl::static_resident_indices(data,size))
  return {.offset=uint32_t(reinterpret_cast<uintptr_t>(resident)),.size=uint32_t(size)};
 // Aurora generated/read these indices before entry. Primitive eligibility and
 // generation time cannot be inferred here; all real allocation/copy calls count.
 twv::vgl::CpuIndexScope cpuCost(sCpuCostProbe,size);
 void* copy=twv_vgl_alloc(size);
 cpuCost.allocated(copy!=nullptr);
 if(!copy) {twv::vgl::count("index_allocation_failure");return {};}
 std::memcpy(copy,data,size);
 cpuCost.copied();
 return {.offset=uint32_t(reinterpret_cast<uintptr_t>(copy)),.size=uint32_t(size)};
}
Range push_uniform(const uint8_t*,size_t size) { return {.size=uint32_t(size)}; }
Vec2<uint32_t> get_render_target_size() noexcept {
 return twv::vgl::offscreen ? Vec2<uint32_t>{twv::vgl::off_w,twv::vgl::off_h} :
                               Vec2<uint32_t>{twv::vgl::GameWidth,twv::vgl::GameHeight};
}
void set_viewport(const Viewport& vp) noexcept { twv::vgl::viewport=vp; }
void set_scissor(const ClipRect& r) noexcept { twv::vgl::scissor=r; }
bool has_normal_attachment() noexcept { return false; }
RenderTargetLayout get_render_target_layout() noexcept {
 RenderTargetLayout layout{.colorAttachmentCount=1,
  .colorAttachments={{{ColorAttachmentSemantic::SceneColor,wgpu::TextureFormat::RGBA8Unorm}}},
  .depthStencilFormat=wgpu::TextureFormat::Depth24Plus,.sampleCount=1};
 detail::finalize_render_target_layout(layout);return layout;
}
template<> PipelineRef pipeline_ref<clear::PipelineConfig>(const clear::PipelineConfig& c) {
 return (c.clearColor?1:0)|(c.clearAlpha?2:0)|(c.clearDepth?4:0);
}
template<> void push_draw_command<clear::DrawData>(clear::DrawData c) {
 if(!twv::vgl::in_frame || twv::vgl::offscreen) return;
 const auto& r=c.rect;
 const float depth=aurora::gx::UseReversedZ ? 1.f-c.depth : c.depth;
 twv_vgl_clear(c.color.r,c.color.g,c.color.b,c.color.a,depth,unsigned(c.pipeline),
  twv::vgl::GameLeft+r.x,544-r.y-r.height,r.width,r.height);
}
template<> PipelineRef pipeline_ref<gx::PipelineConfig>(const gx::PipelineConfig& config) {
 twv::vgl::CpuPipelineScope cpuCost(sCpuCostProbe,sizeof config);
 auto& cache=twv::vgl::pipelines;
 const auto hash=XXH32(&config,sizeof config,0);
 const auto existing=twv::vgl::pipelineIndex.find(hash,config);
 if(existing!=UINT32_MAX) {cpuCost.found();return existing;}
 cpuCost.missing();
 // This exclusive phase includes descriptor initialization/config copy and
 // backend shader info. Aurora's separate frontend build_shader_info is outside.
 twv::vgl::Pipeline p;p.config=config;p.info=gx::build_shader_info(config.shaderConfig);
 cpuCost.shader_info_done();
 // Capability selection, texgen, vertex layout and dependency preparation.
 p.gpuVertex=twv::vgl::gpu_vertex_supported(p);
 twv::vgl::configure_gpu_texgen(p);
 p.vertexLayout=p.gpuVertex ? twv::vgl::gpu_vertex_layout(p) : twv::vgl::legacy_vertex_layout();
 twv::vgl::configure_gpu_cache_dependencies(p);
 cpuCost.layout_done();
 twv::vgl::pipelineIndex.insert(hash,config,uint32_t(cache.size()));
 cache.push_back(std::move(p));cpuCost.created();return cache.size()-1;
}
template<> gx::DrawData* get_last_draw_command<gx::DrawData>() { return nullptr; }
template<> void push_draw_command<gx::DrawData>(gx::DrawData data) {
 using namespace twv::vgl;
 if(!in_frame) {count("draw_outside_frame");return;}
 if(offscreen && !OffscreenDrawSupported) {count("unsupported_offscreen_draw");tally_offscreen_discard();return;}
 if(data.pipeline>=pipelines.size()) {count("invalid_pipeline");return;}
 auto& p=pipelines[data.pipeline]; const auto& c=p.config;
 if(c.cullMode==GX_CULL_ALL) return;
 if(c.shaderConfig.lineMode) {count("unsupported_line_point_draw");return;}
 if(!CopyResolveSupported && samples_unresolved_copy(p)) {count("unresolved_copy_draw");return;}
 if(c.shaderConfig.numIndStages) count(CounterId::FallbackIndirectDraw);
 if(c.shaderConfig.fogRangeEnabled) count(CounterId::FallbackFogRangeDraw);
 if(c.blendMode==GX_BM_LOGIC && c.blendOp!=GX_LO_COPY) count(CounterId::FallbackLogicBlendDraw);
 const auto lights=gpu_light_plan(p);
 // G2 (GXM, --vita-gxm-compact): packed formats are fixed before the first decode and shader.
 if(!p.packedConfigured) configure_packed_formats(p);
 const unsigned variant=lights.count;
 if(!p.attempted[variant]) {
  ProfileScope timer(TWV_PROFILE_SHADER);
  p.attempted[variant]=true;
  twv::vita::telemetryEvent("gfx_pipeline_compile",data.pipeline);
  twv::vita::telemetryEvent("gfx_vertex_stride",p.vertexLayout.stride);
  twv::vita::telemetryEvent("gfx_gpu_texgen_enabled",p.gpuTexgen);
  twv::vita::telemetryEvent("gfx_shader_light_count",variant);
  twv::vita::telemetryEvent("gfx_cache_source_array_mask",p.sourceArrayMask);
  twv::vita::telemetryEvent("gfx_cache_uses_current_pn",p.usesCurrentPn);
  if(p.info.lightingEnabled && lights.count && light_sample_batches<6) {
   ++light_sample_batches;
   for(unsigned i=0;i<lights.count;++i) {
    const auto source=lights.indices[i];const auto& light=aurora::gx::g_gxState.lights[source];
    char sample[224];
    std::snprintf(sample,sizeof sample,"pipeline=%lu source=%u rgba=%.6f,%.6f,%.6f,%.6f masks=%02lx,%02lx,%02lx,%02lx",
      static_cast<unsigned long>(data.pipeline),source,double(light.color[0]),double(light.color[1]),double(light.color[2]),double(light.color[3]),
      static_cast<unsigned long>(lights.masks[0]),static_cast<unsigned long>(lights.masks[1]),
      static_cast<unsigned long>(lights.masks[2]),static_cast<unsigned long>(lights.masks[3]));
    twv::vita::telemetryText("gfx_light_color_sample",sample);
   }
  }
  twv::vita::telemetryEvent("gfx_vertex_uniform_vectors",twv::vgl::vertex_uniform_vectors(p.info.lightingEnabled,p.gpuTexgen?p.texgenVectors:0,variant));
  p.programs[variant]=twv_vgl_program(vertex_shader(p,variant).c_str(),fragment_shader(p).c_str());
  if(!p.programs[variant]) {twv::vita::telemetryEvent("gfx_pipeline_failure",data.pipeline);return;}
 }
 if(!p.programs[variant]) return;
 if(data.indexCount && !data.idxRange.size) {count("missing_indices");return;}
 const auto& vp=viewport; const auto& r=scissor;
 const int x0=std::max({r.x,int(std::floor(vp.left)),0});
 const int y0=std::max({r.y,int(std::floor(vp.top)),0});
 const int x1=std::min({r.x+r.width,int(std::ceil(vp.left+vp.width)),GameWidth});
 const int y1=std::min({r.y+r.height,int(std::ceil(vp.top+vp.height)),544});
 if(x1<=x0||y1<=y0)return;
 const bool trace=!p.drawn;
 if(trace && !p.vertexTraced){twv::vita::telemetryEvent("gfx_vertex_enter",data.pipeline);p.vertexTraced=true;}
 void* vertices;unsigned pnSlots=10;bool vertexCulled=false;
 { DrawProfileScope<TWV_PROFILE_VERTEX> timer;
   vertices=p.gpuVertex ? build_gpu_vertices(p,data,pnSlots,vertexCulled) : build_vertices(p,data); }
 if(vertexCulled){count(CounterId::VertexCullDraws);return;}
 if(!vertices)return;
 if(trace) twv::vita::telemetryEvent("gfx_vertex_return",data.pipeline);
 DrawProfileScope<TWV_PROFILE_STATE> stateTimer;
 TwvVitaDrawState state{};state.program=p.programs[variant];
 state.gpu_vertex=p.gpuVertex;
 state.vertex_layout=p.vertexLayout;
 // G2: native primitive (patches/aurora/0008, only when the GXM backend draws them) and packed formats.
 state.primitive=data.primitive==GX_TRIANGLESTRIP?TwvPrimitiveStrip:data.primitive==GX_TRIANGLEFAN?TwvPrimitiveFan:
     data.primitive==GX_QUADS?TwvPrimitiveQuads:TwvPrimitiveTriangles;
 std::memcpy(state.vertex_formats,p.packed.format.data(),TwvVertexAttributeCount);
 std::memcpy(state.vertex_counts,p.packed.count.data(),TwvVertexAttributeCount);
 std::memcpy(state.vertex_fracs,p.packed.frac.data(),TwvVertexAttributeCount);
 if(p.gpuVertex)gpu_vertex_uniforms(state,p,pnSlots,lights);
 state.viewport[0]=GameLeft+int(vp.left);state.viewport[1]=544-int(vp.top+vp.height);
 state.viewport[2]=int(vp.width);state.viewport[3]=int(vp.height);
 state.scissor[0]=GameLeft+x0;state.scissor[1]=544-y1;state.scissor[2]=x1-x0;state.scissor[3]=y1-y0;
 const auto& gx=aurora::gx::g_gxState;
 for(int col=0;col<4;++col) {
  state.projection[col]=gx.proj.m0[col];state.projection[4+col]=gx.proj.m1[col];
  state.projection[8+col]=2.f*(vp.zfar-vp.znear)*gx.proj.m2[col]+(2.f*vp.zfar-1.f)*gx.proj.m3[col];
  state.projection[12+col]=gx.proj.m3[col];
 }
 for(int i=0;i<4;++i)for(int j=0;j<4;++j){state.color_regs[i*4+j]=gx.colorRegs[i][j];state.kcolors[i*4+j]=gx.kcolors[i][j];}
 for(int j=0;j<4;++j)state.fog[j]=gx.fog.color[j];
 state.fog[4]=gx.fog.a;state.fog[5]=gx.fog.b;state.fog[6]=gx.fog.c;
 state.depth_test=c.depthCompare;state.depth_write=c.depthCompare&&c.depthUpdate;
 state.depth_func=c.depthFunc;state.cull=c.cullMode;
 state.blend=c.blendMode==GX_BM_BLEND||c.blendMode==GX_BM_SUBTRACT;
 state.src_factor=c.blendFacSrc;state.dst_factor=c.blendFacDst;state.subtract=c.blendMode==GX_BM_SUBTRACT;
 state.color_write=c.colorUpdate;state.alpha_write=c.alphaUpdate;
 p.drawn=true;
 if(trace) twv::vita::telemetryEvent("gfx_bind_enter",data.pipeline);
 for(int i=0;i<8;++i) if(p.info.sampledTextures.test(i)) {
  const auto& bind=gx.textures[i];
  if(!bind.ref) {count("missing_texture_draw");continue;}
  auto* texture=static_cast<Texture*>(bind.ref.get());state.textures[i]=texture->gpu;
  if(!texture->gpu) count("missing_texture_storage_draw");
  const auto& obj=bind.texObj;
  if(!twv_vgl_texture_sampler_unit(i,obj.min_filter(),obj.mag_filter()==GX_LINEAR,obj.wrap_s(),obj.wrap_t())) {
   twv::vita::telemetryText("gfx_bundle_failure","per-unit sampler setup failed before draw");
   twv::vita::telemetryFlush();std::abort();
  }
 }
 stateTimer.finish();
 if(p.gpuVertex) {
  count(CounterId::GpuMatrixSlots,pnSlots);count(CounterId::GpuMatrixSlotsFull,10);
  if(p.info.lightingEnabled) {
   count(CounterId::GpuActiveLights,lights.count);count(CounterId::GpuFullLights,8);count(CounterId::GpuLitDraws);
  }
  if(pnSlots<10)count(CounterId::GpuMatrixPrefixDraws);
 }
 const auto* indices=reinterpret_cast<const uint16_t*>(uintptr_t(data.idxRange.offset));
 { DrawProfileScope<TWV_PROFILE_SUBMIT> timer;
   twv_vgl_draw(&state,vertices,data.vtxCount,indices,data.indexCount); }
 if(trace) twv::vita::telemetryEvent("gfx_draw_return",data.pipeline);
 count(CounterId::DrawsSubmitted);count(CounterId::VerticesSubmitted,data.vtxCount);++sFrameStats.drawCallCount;
 count(p.gpuVertex?CounterId::GpuVertexDraws:CounterId::CpuVertexDraws);
 count(p.gpuVertex?CounterId::GpuVertexVertices:CounterId::CpuVertexVertices,data.vtxCount);
 count(p.gpuTexgen?CounterId::GpuTexgenDraws:CounterId::CpuTexgenDraws);
 count(p.gpuTexgen?CounterId::GpuTexgenVertices:CounterId::CpuTexgenVertices,data.vtxCount);
}
void resolve_pass_into(TextureHandle,ClipRect,bool,bool,bool,Vec4<float>,float,GXTexFmt,GXPixelFmt) {twv::vgl::count("unsupported_efb_copy");}
void begin_offscreen(uint32_t w,uint32_t h) {twv::vgl::off_w=w;twv::vgl::off_h=h;twv::vgl::offscreen=true;}
void end_offscreen() {twv::vgl::offscreen=false;}
bool is_offscreen() noexcept {return twv::vgl::offscreen;}
void push_debug_group(std::string){}
void insert_debug_marker(std::string){}
}
void push_debug_group(const char*){}
void pop_debug_group(){}
// --- Depth peek, copy/palette conversion, texture replacements ---
namespace aurora::gfx::depth_peek {
void initialize() {}
void shutdown() {}
void request_snapshot() noexcept {}
void poll() noexcept {}
void encode_frame_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& depthView,
                           wgpu::Extent3D sourceSize, uint32_t msaaSamples) noexcept {
  (void)cmd, (void)depthView, (void)sourceSize, (void)msaaSamples;
}
void after_submit() noexcept {}
bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  (void)x, (void)y, (void)z;
  return false;
}
} // namespace aurora::gfx::depth_peek

namespace aurora::gfx::tex_copy_conv {
bool needs_conversion(GXTexFmt fmt) {
  (void)fmt;
  return false;
}
} // namespace aurora::gfx::tex_copy_conv

namespace aurora::gfx::tex_palette_conv {
void queue(ConvRequest req) { (void)req; }
} // namespace aurora::gfx::tex_palette_conv

namespace aurora::gfx::texture_replacement {
u32 compute_texture_upload_size(const GXTexObj_& obj) noexcept {
  (void)obj;
  return 0;
}
bool has_replacement(const GXTexObj_&) noexcept { return false; }
bool has_replacement(const GXTexObj_&, const GXTlutObj_&) noexcept { return false; }
} // namespace aurora::gfx::texture_replacement

// --- Texture replacement registry (include/aurora/texture.hpp) ---
namespace aurora::texture {
namespace {
constexpr Module Log{"aurora::gfx_vgl"};
void log_unavailable_once(std::string_view what) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    Log.info("Vita texture replacement extras are not implemented; {} registers nothing. Original disc textures remain active.", what);
  }
}
} // namespace

ReplacementRegistration register_virtual_replacement(std::string_view path, VirtualFileSource source,
                                                     ReplacementOptions options) {
  (void)source, (void)options;
  log_unavailable_once(fmt::format("virtual replacement '{}'", path));
  return {};
}
ReplacementRegistration register_replacement(ReplacementKey key, RawTextureReplacement replacement,
                                             ReplacementOptions options) {
  (void)options;
  log_unavailable_once(fmt::format("replacement '{}'", replacement.label));
  return {.id = 0, .key = std::move(key)}; // id 0: nothing registered
}
void unregister_replacement(const ReplacementRegistration& registration) { (void)registration; }
void unregister_replacements(const ReplacementGroup& group) { (void)group; }
ReplacementGroup load_replacement_directory(const std::filesystem::path& root, ReplacementOptions options) {
  (void)options;
  log_unavailable_once(fmt::format("directory '{}'", io::fs_path_to_string(root)));
  return {};
}
bool has_replacement(const GXTexObj* obj, const GXTlutObj* tlut) {
  (void)obj, (void)tlut;
  return false;
}
} // namespace aurora::texture

// Frame statistics (lib/gfx/frame.cpp's C API): the last completed frame.
const AuroraStats* aurora_get_stats() { return &aurora::gfx::detail::resources().stats; }
