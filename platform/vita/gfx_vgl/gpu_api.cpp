// SDK domain: short enums, no game/Aurora headers. Serialized by FIFO frame drain.
#include "gpu_api.h"
#include "scratch_alloc.hpp"
#include "staging_policy.hpp"
#include "write_class_cache.hpp"
#include "vertex_contract.hpp"
#include "cache_budget.hpp"
#include "sampler_state.hpp"
#include "uniform_state.hpp"
#include "frame_pacing.hpp"
#include "profile_scope.hpp"
#include "raster_state.hpp"
#include "shader_stage_pool.hpp"
#include "shader_preparation.hpp"
#include "bundle_options.h"
#include "memory_policy.hpp"
#include "submit_batch.hpp"
#include "resident_geometry_api.h"
#include "texture_upload_api.h"
#include "texture_upload.hpp"
#include "texture_conversion_cache.hpp"
#include "runtime/telemetry.hpp"
#include "runtime/system_probe.hpp"
#include "runtime/run_control.hpp"
#include "audio/audio_out.hpp"
#include <vitaGL.h>
// Pinned vitaGL7635e3c private memory classifier (utils/mem_utils.h). Keep its
// actual public enum type in the SDK domain; only a uint32 boolean crosses out.
extern "C" vglMemType vgl_mem_get_type_by_addr(void*);
// Local pinned vitaGL patch: reflected snapshot bytes and actual SDK attempts.
// Sparse clear/scissor/blit setters count calls, not full snapshot bytes.
extern "C" void vglGetUniformReservationStats(uint64_t*,uint64_t*,uint64_t*,uint64_t*,uint64_t*);
extern "C" void vglGetUniformReservationDiagnostics(uint64_t*,uint64_t*,uint64_t*,uint64_t*,
    uint32_t*,int32_t*,uint32_t*,uint32_t*,int32_t*,uint32_t*);
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <png.h>
#include <vector>
#include <array>
#include <map>
#include <string>
#include <algorithm>
#include <malloc.h>
extern "C" unsigned int _newlib_heap_size_user;
#ifndef TWV_VITA_CAPTURE_FRAMES
#define TWV_VITA_CAPTURE_FRAMES 0
#endif

namespace { int request_unit_sampler(uint32_t,int,int,int,int); }
extern "C" int twv_vgl_texture_sampler_unit(uint32_t unit,int min,int mag,int s,int t) {
    return request_unit_sampler(unit,min,mag,s,t);
}

namespace {
bool ready = false;
twv::vgl::PersistentRamWriteClass vertexWriteClassCache;
uint64_t vertexWriteOwner=1,vertexWriteQueries=0,vertexWriteHits=0,vertexWriteErrors=0;
int32_t vertexWriteLastError=0;
uint32_t frames = 0, draws = 0, errors = 0;
GLuint vertexBuffer = 0, indexBuffer = 0, activeProgram = 0;
using ProgramKey = std::pair<std::string,std::string>;
struct ProgramInfo {
    std::array<GLint,4> uniforms{{-1,-1,-1,-1}};
    std::array<GLint,8> samplers{{-1,-1,-1,-1,-1,-1,-1,-1}};
    std::array<GLint,11> vertexUniforms{{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}};
    ProgramKey key;
    std::array<GLuint,2> stages{};
    uint32_t references = 0;
};
std::vector<ProgramInfo> programs;
std::map<ProgramKey,GLuint> programCache;
uint32_t programCacheHits = 0, uniquePrograms = 0;
twv::vgl::ShaderStagePool<> shaderStagePool;
uint64_t shaderStageRequests=0,shaderCompileCalls=0;
std::array<uint64_t,TWV_PROFILE_COUNT> profileUs{};
twv::vgl::FramePacing framePacing;
uint64_t directVertexBytes = 0, directIndexBytes = 0;
TwvVitaVertexLayout activeLayout{};
twv::vgl::TextureSamplerCache textureSamplerCache;
uint64_t samplerRequests=0,samplerUpdates=0,samplerSkips=0;
// C03_SAMPLER_UNIT_STATE_BEGIN
std::array<GLuint,8> unitSamplers{};
twv::vgl::UnitSamplerCache unitSamplerCache;
bool unitSamplersReady=false,unitSamplerPendingFailure=false;
uint32_t unitSamplerPendingMask=0;
uint64_t unitSamplerRequests=0,unitSamplerUpdates=0,unitSamplerSkips=0,unitSamplerFailures=0;
uint64_t unitSamplerGlErrors=0,unitSamplerCreates=0,unitSamplerDeletes=0;
uint64_t activeSamplerAliasDraws=0,activeSamplerAliasPairs=0;
struct SamplerAliasExample {
    uint32_t frame=0,program=0,texture=0,first=0,second=0;
    twv::vgl::SamplerParams a{},b{};
};
std::array<SamplerAliasExample,4> samplerAliasExamples{};
unsigned samplerAliasExampleCount=0,samplerAliasExamplesReported=0;
void unit_sampler_receipts();
// C03_SAMPLER_UNIT_STATE_END
twv::vgl::ProgramUniformCache programUniformCache;
uint64_t uniformRequests=0,uniformUpdates=0,uniformSkips=0,uniformUploadBytes=0,uniformSkippedBytes=0;
twv::vgl::RasterShadow rasterShadow;
uint64_t rasterRequests=0,rasterUpdates=0;
twv::vita::MemoryPolicy gpuMemoryPolicy;
twv::vita::SubmitObserver submitObserver;
bool submitObserverEnabled=false;
uint64_t textureZeroInitBytes=0,textureDirectInitBytes=0;
void submit_barrier(){if(submitObserverEnabled)submitObserver.barrier();}
struct CpuHeapUsage {uint32_t valid=0,used=0,arena=0;};
CpuHeapUsage cpu_heap_usage() {
    const auto info=mallinfo();
    CpuHeapUsage result;
    if(info.arena>0 && info.uordblks>=0 && info.hblkhd>=0 && info.uordblks<=info.arena) {
        const uint64_t used=uint64_t(info.uordblks)+uint64_t(info.hblkhd);
        if(used<=UINT32_MAX)result={1,uint32_t(used),uint32_t(info.arena)};
    }
    return result;
}
GLenum wrap(int n);
// C03_SAMPLER_UNIT_HELPERS_BEGIN
bool unit_sampler_gl_ok(unsigned phase) {
    const auto error=glGetError();
    if(error==GL_NO_ERROR)return true;
    // Do not silently drain a pre-existing GL error. Preserve its code in the
    // ordinary error total and bounded explicit failure receipts, then fail.
    ++errors;++unitSamplerGlErrors;
    if(unitSamplerGlErrors<=8) {
        twv::vita::telemetryEvent("gfx_sampler_unit_gl_error",error);
        twv::vita::telemetryEvent("gfx_sampler_unit_gl_phase",phase);
    }
    return false;
}
void release_unit_samplers() {
    // Only private, nonzero owned handles are ever passed to the driver.
    for(unsigned unit=0;unit<unitSamplers.size();++unit)if(unitSamplers[unit]) {
        glBindSampler(unit,0);
        glDeleteSamplers(1,&unitSamplers[unit]);
        unitSamplers[unit]=0;++unitSamplerDeletes;
    }
    unitSamplersReady=false;unitSamplerCache.clear();
    unitSamplerPendingMask=0;unitSamplerPendingFailure=false;
}
bool initialize_unit_samplers() {
    if(unitSamplersReady)return true;
    unitSamplers.fill(0);unitSamplerCache.clear();
    unitSamplerPendingMask=0;unitSamplerPendingFailure=false;
    // Requires the separately drafted transactional glGenSamplers OOM guard.
    if(!unit_sampler_gl_ok(1)){++unitSamplerFailures;return false;}
    glGenSamplers(int(unitSamplers.size()),unitSamplers.data());
    bool complete=unit_sampler_gl_ok(2);
    for(auto sampler:unitSamplers)if(!sampler)complete=false;else ++unitSamplerCreates;
    if(complete) {
        for(auto sampler:unitSamplers)glSamplerParameteri(sampler,GL_TEXTURE_LOD_BIAS,0);
        complete=unit_sampler_gl_ok(3);
    }
    if(!complete){++unitSamplerFailures;release_unit_samplers();return false;}
    unitSamplersReady=true;return true;
}
int request_unit_sampler(uint32_t unit,int min,int mag,int s,int t) {
    ++unitSamplerRequests;
    if(!ready || !unitSamplersReady || unit>=unitSamplers.size() || !unitSamplers[unit]) {
        ++unitSamplerFailures;unitSamplerPendingFailure=true;return 0;
    }
    const auto params=twv::vgl::canonical_sampler_params(min,mag,s,t);
#if TWV_VITA_SAMPLER_CACHE
    if(unitSamplerCache.unchanged(unit,params)) {
        ++unitSamplerSkips;unitSamplerPendingMask|=1u<<unit;return 1;
    }
#endif
    submit_barrier();
    if(!unit_sampler_gl_ok(4)) {
        ++unitSamplerFailures;unitSamplerPendingMask&=~(1u<<unit);unitSamplerPendingFailure=true;return 0;
    }
    const GLenum filters[]={GL_NEAREST,GL_LINEAR,GL_NEAREST_MIPMAP_NEAREST,
        GL_LINEAR_MIPMAP_NEAREST,GL_NEAREST_MIPMAP_LINEAR,GL_LINEAR_MIPMAP_LINEAR};
    const auto sampler=unitSamplers[unit];
    glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,filters[params.min]);
    glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,params.mag?GL_LINEAR:GL_NEAREST);
    glSamplerParameteri(sampler,GL_TEXTURE_WRAP_S,wrap(params.s));
    glSamplerParameteri(sampler,GL_TEXTURE_WRAP_T,wrap(params.t));
    if(!unit_sampler_gl_ok(5)) {
        ++unitSamplerFailures;unitSamplerCache.invalidate(unit);unitSamplerPendingMask&=~(1u<<unit);
        unitSamplerPendingFailure=true;return 0;
    }
    // Retain effective values in both cache modes for exact alias detection.
    unitSamplerCache.applied(unit,params);++unitSamplerUpdates;
    unitSamplerPendingMask|=1u<<unit;return 1;
}
uint32_t consume_unit_sampler_requests() {
    const auto mask=unitSamplerPendingMask;unitSamplerPendingMask=0;
    const bool failed=unitSamplerPendingFailure;unitSamplerPendingFailure=false;
    if(failed) {
        twv::vita::telemetryText("gfx_sampler_unit_failure","failed current-draw sampler request; submission stopped");
        twv::vita::telemetryFlush();std::abort();
    }
    return mask;
}
void bind_draw_sampler(unsigned unit,uint32_t mask) {
    // Always rebind active units. Helpers may have changed GL bindings since
    // the preceding draw; no binding shadow is trusted across that boundary.
    glBindSampler(unit,(mask&(1u<<unit))?unitSamplers[unit]:0);
}
void record_active_sampler_aliases(const ProgramInfo& program,const TwvVitaDrawState* state,uint32_t mask) {
    bool conflict=false;
    for(unsigned a=0;a<8;++a)if((mask&(1u<<a)) && program.samplers[a]>=0 && state->textures[a]) {
        twv::vgl::SamplerParams first{};
        if(!unitSamplerCache.parameters(a,first))continue;
        for(unsigned b=a+1;b<8;++b)if((mask&(1u<<b)) && program.samplers[b]>=0 && state->textures[b]==state->textures[a]) {
            twv::vgl::SamplerParams second{};
            if(!unitSamplerCache.parameters(b,second) || first==second)continue;
            conflict=true;++activeSamplerAliasPairs;
            if(samplerAliasExampleCount<samplerAliasExamples.size())
                samplerAliasExamples[samplerAliasExampleCount++]={frames,state->program,state->textures[a],a,b,first,second};
        }
    }
    if(conflict)++activeSamplerAliasDraws;
}
void unit_sampler_receipts() {
    twv::vita::telemetryEvent("gfx_sampler_unit_initialized",unitSamplersReady);
    twv::vita::telemetryEvent("gfx_sampler_unit_requests",unitSamplerRequests);
    twv::vita::telemetryEvent("gfx_sampler_unit_updates",unitSamplerUpdates);
    twv::vita::telemetryEvent("gfx_sampler_unit_skips",unitSamplerSkips);
    twv::vita::telemetryEvent("gfx_sampler_unit_failures",unitSamplerFailures);
    twv::vita::telemetryEvent("gfx_sampler_unit_gl_errors",unitSamplerGlErrors);
    twv::vita::telemetryEvent("gfx_sampler_unit_creates",unitSamplerCreates);
    twv::vita::telemetryEvent("gfx_sampler_unit_deletes",unitSamplerDeletes);
    twv::vita::telemetryEvent("gfx_sampler_active_alias_draws",activeSamplerAliasDraws);
    twv::vita::telemetryEvent("gfx_sampler_active_alias_pairs",activeSamplerAliasPairs);
    twv::vita::telemetryEvent("gfx_sampler_active_alias_examples",samplerAliasExampleCount);
    while(samplerAliasExamplesReported<samplerAliasExampleCount) {
        const auto& e=samplerAliasExamples[samplerAliasExamplesReported++];
        char text[256];
        std::snprintf(text,sizeof text,"frame=%lu program=%lu texture=%lu units=%lu,%lu a=%d,%d,%d,%d b=%d,%d,%d,%d",
            static_cast<unsigned long>(e.frame),static_cast<unsigned long>(e.program),static_cast<unsigned long>(e.texture),
            static_cast<unsigned long>(e.first),static_cast<unsigned long>(e.second),e.a.min,e.a.mag,e.a.s,e.a.t,e.b.min,e.b.mag,e.b.s,e.b.t);
        twv::vita::telemetryText("gfx_sampler_active_alias_example",text);
    }
}
// C03_SAMPLER_UNIT_HELPERS_END
void profile_receipts(uint64_t now) {
    twv::vgl::FramePacingStats pacing;
    if(!framePacing.frame_end(now,pacing))return;
    const char* names[] = {"prof_vertex_cpu_us", "prof_gl_submit_us", "prof_shader_us",
        "prof_state_us", "prof_pipeline_us", "prof_present_us",
        "prof_fifo_inclusive_us", "prof_game_execute_inclusive_us", "prof_game_audio_us",
        "prof_vi_callbacks_us", "prof_painter_inclusive_us", "prof_events_us"};
    twv::vita::telemetryEvent("prof_frame_samples",pacing.frames);
    twv::vita::telemetryEvent("prof_draw_timers_enabled",TWV_VITA_DRAW_TIMERS);
    twv::vita::telemetryEvent("prof_interval_samples",pacing.intervals);
    twv::vita::telemetryEvent("prof_interval_rejected",pacing.rejected);
    twv::vita::telemetryEvent("prof_frame_interval_us",pacing.meanUs);
    twv::vita::telemetryEvent("prof_frame_min_us",pacing.minUs);
    twv::vita::telemetryEvent("prof_frame_p50_us",pacing.p50Us);
    twv::vita::telemetryEvent("prof_frame_p95_us",pacing.p95Us);
    twv::vita::telemetryEvent("prof_frame_p99_us",pacing.p99Us);
    twv::vita::telemetryEvent("prof_frame_max_us",pacing.maxUs);
    twv::vita::telemetryEvent("prof_frame_over_33334us",pacing.overBudget);
    // One bounded group per existing 30-frame report, never per frame. Text
    // preserves full unsigned timestamps/long intervals through signed EventLog.
    // Repeating the small schema lets a complete extracted window stand alone.
    char histogram[256];
    std::snprintf(histogram,sizeof histogram,
        "v=1;unit=us;e=%llu;s=%llu;f=%lu;a=%lu;r=%lu;t0=%llu;t1=%llu;x=%llu",
        static_cast<unsigned long long>(pacing.epoch),
        static_cast<unsigned long long>(pacing.sequence),
        static_cast<unsigned long>(pacing.frames),
        static_cast<unsigned long>(pacing.intervals),
        static_cast<unsigned long>(pacing.rejected),
        static_cast<unsigned long long>(pacing.startUs),
        static_cast<unsigned long long>(pacing.endUs),
        static_cast<unsigned long long>(pacing.maxUs));
    twv::vita::telemetryText("prof_interval_hist_begin",histogram);
    twv::vita::telemetryText("prof_interval_hist_bounds_us",twv::vgl::FrameIntervalBoundsText);
    size_t histogramUsed=0;
    for(size_t i=0;i<pacing.histogram.size();++i)
        histogramUsed+=size_t(std::snprintf(histogram+histogramUsed,sizeof histogram-histogramUsed,
            "%s%lu",i?",":"",static_cast<unsigned long>(pacing.histogram[i])));
    twv::vita::telemetryText("prof_interval_hist_counts",histogram);
    std::snprintf(histogram,sizeof histogram,"e=%llu;s=%llu",
        static_cast<unsigned long long>(pacing.epoch),static_cast<unsigned long long>(pacing.sequence));
    twv::vita::telemetryText("prof_interval_hist_end",histogram);
    for(size_t i=0;i<profileUs.size();++i) {
        if(twv::vgl::profile_category_enabled(i))
            twv::vita::telemetryEvent(names[i],profileUs[i]/pacing.frames);
        profileUs[i]=0;
    }
    twv::vita::telemetryEvent("gfx_program_cache_hits",programCacheHits);
    twv::vita::telemetryEvent("gfx_unique_programs",uniquePrograms);
    twv::vita::telemetryEvent("gfx_shader_stage_requests",shaderStageRequests);
    twv::vita::telemetryEvent("gfx_shader_stage_compile_calls",shaderCompileCalls);
    twv::vita::telemetryEvent("gfx_shader_stage_cache_hits",shaderStagePool.stats.hits);
    twv::vita::telemetryEvent("gfx_shader_stage_cache_fallbacks",shaderStagePool.stats.fallbacks);
    twv::vita::telemetryEvent("gfx_shader_stage_cache_entries",shaderStagePool.live_entries());
    twv::vita::telemetryEvent("gfx_shader_stage_source_bytes",shaderStagePool.source_bytes());
    twv::vita::telemetryEvent("gfx_shader_stage_source_peak_bytes",shaderStagePool.stats.peakSourceBytes);
    twv::vita::telemetryEvent("gfx_direct_vertex_bytes",directVertexBytes);
    twv::vita::telemetryEvent("gfx_direct_index_bytes",directIndexBytes);
    twv::vita::telemetryEvent("gfx_sampler_requests",samplerRequests);
    twv::vita::telemetryEvent("gfx_sampler_updates",samplerUpdates);
    twv::vita::telemetryEvent("gfx_sampler_skips",samplerSkips);
    unit_sampler_receipts();
    twv::vita::telemetryEvent("gfx_uniform_requests",uniformRequests);
    twv::vita::telemetryEvent("gfx_uniform_updates",uniformUpdates);
    twv::vita::telemetryEvent("gfx_uniform_skips",uniformSkips);
    twv::vita::telemetryEvent("gfx_uniform_upload_bytes",uniformUploadBytes);
    twv::vita::telemetryEvent("gfx_uniform_skipped_bytes",uniformSkippedBytes);
    twv::vita::telemetryEvent("gfx_uniform_cache_bytes",sizeof programUniformCache);
    twv::vita::telemetryEvent("gfx_raster_requests",rasterRequests);
    twv::vita::telemetryEvent("gfx_raster_updates",rasterUpdates);
    twv::vita::telemetryEvent("gfx_raster_skips",rasterRequests-rasterUpdates);
    const auto audio=twv::vita::audio::stats();
    twv::vita::telemetryEvent("audio_buffers",audio.buffers);
    twv::vita::telemetryEvent("audio_late_buffers",audio.late);
    twv::vita::telemetryEvent("audio_max_render_us",audio.maxRenderUs);
    twv::vita::telemetryFlush();
}
void capture_frame() {
#if TWV_VITA_CAPTURE_FRAMES
    // Actual framebuffer readback, never a CPU reference render. Bounded to
    // eight images; disabled in ordinary builds to avoid SD/CPU overhead.
    if(frames<60 || frames>270 || frames%30) return;
    std::vector<unsigned char> pixels(960*544*4);
    glReadPixels(0,0,960,544,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    char path[192]; std::snprintf(path,sizeof path,"ux0:data/twilight-vita/frame-%06lu.png",static_cast<unsigned long>(frames));
    FILE* file=std::fopen(path,"wb");
    if(!file) {twv::vita::telemetryText("capture_error",path);return;}
    png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
    png_infop info=png?png_create_info_struct(png):nullptr;
    if(!png || !info || setjmp(png_jmpbuf(png))) {
        if(png)png_destroy_write_struct(&png,info?&info:nullptr);
        std::fclose(file); twv::vita::telemetryText("capture_error",path);return;
    }
    png_init_io(png,file);
    png_set_IHDR(png,info,960,544,8,PNG_COLOR_TYPE_RGBA,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png,info);
    for(int y=543;y>=0;--y)png_write_row(png,pixels.data()+size_t(y)*960*4);
    png_write_end(png,info);png_destroy_write_struct(&png,&info);std::fclose(file);
    twv::vita::telemetryText("frame_capture",path);
#endif
}
void memory_receipts() {
    const auto heap=cpu_heap_usage();
    twv::vita::telemetryEvent("cpu_heap_stats_valid",heap.valid);
    twv::vita::telemetryEvent("cpu_heap_limit_bytes",_newlib_heap_size_user);
    if(heap.valid) {
        twv::vita::telemetryEvent("cpu_heap_used_bytes",heap.used);
        twv::vita::telemetryEvent("cpu_heap_arena_bytes",heap.arena);
    }
    // vglMemFree uses sceClibMspaceMallocStats, unimplemented by the pinned
    // emulator and leaving its output uninitialized. Query checked kernel
    // totals instead; these are system headroom, not per-pool high-water marks.
    SceKernelFreeMemorySizeInfo info{};
    static_assert(sizeof info==0x10);
    info.size=sizeof info;
    if(sceKernelGetFreeMemorySize(&info)==0) {
        twv::vita::telemetryEvent("gfx_system_user_free_kib",info.size_user/1024);
        twv::vita::telemetryEvent("gfx_system_cdram_free_kib",info.size_cdram/1024);
    }
}
GLenum factor(int n, bool destination) {
    const GLenum values[] = {GL_ZERO, GL_ONE, GLenum(destination ? GL_SRC_COLOR : GL_DST_COLOR),
        GLenum(destination ? GL_ONE_MINUS_SRC_COLOR : GL_ONE_MINUS_DST_COLOR),
        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return n >= 0 && n < 8 ? values[n] : GL_ONE;
}
void submit_raster(const TwvVitaDrawState* s) {
    using namespace twv::vgl;
    uint32_t flags=RasterAll;
#if TWV_VITA_RASTER_CACHE
    const auto state=raster_state(*s);flags=rasterShadow.changes(state);
#endif
    const auto call=[&](uint32_t flag,auto action) {
        ++rasterRequests;if(flags&flag){action();++rasterUpdates;}
    };
    call(RasterViewport,[&]{glViewport(s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3]);});
    call(RasterScissorTest,[&]{glEnable(GL_SCISSOR_TEST);});
    call(RasterScissor,[&]{glScissor(s->scissor[0],s->scissor[1],s->scissor[2],s->scissor[3]);});
    call(RasterDepthTest,[&]{if(s->depth_test)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);});
    call(RasterDepthFunc,[&]{glDepthFunc(GL_NEVER+s->depth_func);});
    call(RasterDepthWrite,[&]{glDepthMask(s->depth_write?GL_TRUE:GL_FALSE);});
    call(RasterColorMask,[&]{glColorMask(s->color_write,s->color_write,s->color_write,s->alpha_write);});
    call(RasterCull,[&]{if(s->cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);});
    if(s->cull)call(RasterCull,[&]{glCullFace(s->cull==1?GL_FRONT:GL_BACK);});
    call(RasterBlend,[&]{if(s->blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);});
    if(s->blend) {
        call(RasterBlend,[&]{glBlendFunc(s->subtract?GL_ONE:factor(s->src_factor,false),s->subtract?GL_ONE:factor(s->dst_factor,true));});
        call(RasterBlend,[&]{glBlendEquation(s->subtract?GL_FUNC_REVERSE_SUBTRACT:GL_FUNC_ADD);});
    }
#if TWV_VITA_RASTER_CACHE
    rasterShadow.applied(state);
#endif
}
GLenum wrap(int n) { return n == 1 ? GL_REPEAT : n == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE; }
void uniform4(twv::vgl::ProgramUniforms* shadow,unsigned field,GLint location,int count,const float* values) {
    if(location<0 || count<=0)return;
    ++uniformRequests;
    if(shadow && shadow->unchanged(field,location,unsigned(count),values)) {
        ++uniformSkips;uniformSkippedBytes+=uint64_t(count)*16;return;
    }
    glUniform4fv(location,count,values);
    ++uniformUpdates;uniformUploadBytes+=uint64_t(count)*16;
    if(shadow)shadow->applied(field,location,unsigned(count),values);
}
void submit_uniforms(const ProgramInfo& program,const TwvVitaDrawState* s) {
    twv::vgl::ProgramUniforms* shadow=nullptr;
#if TWV_VITA_UNIFORM_CACHE
    shadow=programUniformCache.select(s->program);
#endif
    uniform4(shadow,0,program.uniforms[0],4,s->projection);
    uniform4(shadow,1,program.uniforms[1],4,s->color_regs);
    uniform4(shadow,2,program.uniforms[2],4,s->kcolors);
    uniform4(shadow,3,program.uniforms[3],2,s->fog);
    if(s->gpu_vertex) {
        const float* values[]={s->pn_pos,s->pn_nrm,s->light_pos,s->light_dir,s->light_color,
            s->light_cos,s->light_dist,s->channel_mat,s->channel_amb,s->light_masks};
        const int n=int(s->light_count);
        const int counts[]={int(s->pn_vectors),int(s->pn_vectors),n,n,n,n,n,4,4,8};
        for(int i=0;i<10;++i)uniform4(shadow,4+i,program.vertexUniforms[i],counts[i],values[i]);
        if(s->texgen_vectors)uniform4(shadow,14,program.vertexUniforms[10],s->texgen_vectors,s->texgen_rows);
    }
}
GLuint compile_native(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char error[2048]{};
        glGetShaderInfoLog(shader, sizeof error, nullptr, error);
        twv::vita::telemetryText("shader_compile_failure", error);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
GLuint compile(GLenum type,const char* source) {
    ++shaderCompileCalls;
    twv::vgl::capture_shader_source(type==GL_CG_VERTEX_SHADER_EXT?
        twv::vgl::VertexSourceTag:twv::vgl::FragmentSourceTag,source);
    const uint64_t start=twv_vgl_time_us();
    const GLuint shader=compile_native(type,source);
    twv::vgl::observe_shader_compile(twv_vgl_time_us()-start);
    return shader;
}
uint32_t prepare_compile(uint32_t stage,const char* source) {
    return compile_native(stage==twv::vgl::VertexSourceTag?
        GL_CG_VERTEX_SHADER_EXT:GL_CG_FRAGMENT_SHADER_EXT,source);
}
void prepare_release(uint32_t shader) {glDeleteShader(shader);}
GLuint acquire_shader(GLenum type,const char* source) {
    ++shaderStageRequests;
#if TWV_VITA_SHADER_STAGE_CACHE
    return shaderStagePool.acquire(type,source,compile);
#else
    return compile(type,source);
#endif
}
void release_shader(GLuint shader) {
#if TWV_VITA_SHADER_STAGE_CACHE
    shaderStagePool.release(shader,[](uint32_t id){glDeleteShader(id);});
#else
    if(shader)glDeleteShader(shader);
#endif
}
}

extern "C" int twv_vgl_init() {
    submitObserver.reset();
    submitObserverEnabled=twv_vita_bundle_feature(TWV_BUNDLE_SUBMIT_OBSERVER)!=0;
    const bool resident=twv_vita_bundle_feature(TWV_BUNDLE_RESIDENT_GEOMETRY)!=0;
    if(!twv::vita::memory_policy(twv_vita_bundle_memory_profile(),resident?TWV_RESIDENT_GEOMETRY_RESERVE:0,gpuMemoryPolicy)){
        twv::vita::telemetryText("gfx_bundle_failure","invalid memory policy");return 0;
    }
    twv_vgl_texture_conversion_cache_clear();
    textureSamplerCache.clear();
    programUniformCache.clear();
    framePacing.reset();profileUs.fill(0);
    rasterShadow.invalidate();
    // Two bounded GPU pools, independent of the 160 MiB CPU/newlib reservation.
    // No MSAA during bring-up. Compiler errors are fatal evidence, never a fake PASS.
    vglSetShaderCachePath("ux0:data/twilight-vita/shader_cache");
    // Measured house GX stream uses ~8.7 MiB of transient vertices/indices per
    // frame. vitaGL divides this ring across three display buffers: reserve
    // 16 MiB each, rather than overrunning the library's 8 MiB total default.
    vglSetCircularPoolSize(uint32_t(gpuMemoryPolicy.scratch_bytes));
    // The pinned vitaGL return is "resolution fallback occurred", NOT success.
    // Zero is normal for the requested supported 960x544 display mode.
    const auto resolution_fallback = vglInitWithCustomSizes(0, 960, 544, uint32_t(gpuMemoryPolicy.ram_bytes),
                                   uint32_t(gpuMemoryPolicy.cdram_bytes), 0, 0, SCE_GXM_MULTISAMPLE_NONE);
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT,viewport);
    ready = viewport[2]==960 && viewport[3]==544;
    if(ready && !initialize_unit_samplers())ready=false;
    twv::vita::telemetryEvent("gfx_resolution_fallback",resolution_fallback);
    twv::vita::telemetryEvent("gfx_initialized", ready);
    twv::vita::telemetryText("gfx_backend", "vitaGL/Cg on GXM; initial 4:3 comparison viewport");
    twv::vita::telemetryEvent("gfx_scratch_alignment", twv::vgl::ScratchAlignment);
    if (!ready) return 0;
    twv::vita::telemetryEvent("gfx_resident_init",resident?twv_vgl_resident_init():0);
    twv::vita::telemetryEvent("gfx_bundle_features",twv_vita_bundle_features());
    twv::vita::telemetryEvent("gfx_bundle_memory_profile",twv_vita_bundle_memory_profile());
    twv::vita::telemetryEvent("gfx_bundle_ram_bytes",gpuMemoryPolicy.ram_bytes);
    twv::vita::telemetryEvent("gfx_bundle_cdram_bytes",gpuMemoryPolicy.cdram_bytes);
    twv::vita::telemetryEvent("gfx_bundle_resident_reserve",gpuMemoryPolicy.resident_bytes);
    twv::vita::telemetryEvent("gfx_bundle_total_bytes",gpuMemoryPolicy.total_bytes);
    // Scratch memory is already GXM mapped and lives for three display buffers.
    // Stable VBO descriptors borrow that storage; no second driver-side copy.
    // Fixed offsets also let vitaGL reuse its native vertex-layout cache.
    glGenBuffers(1,&vertexBuffer); glGenBuffers(1,&indexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER,vertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indexBuffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(TwvVitaVertex),nullptr);
    for (int i=0;i<2;++i) {
        glEnableVertexAttribArray(1+i);
        glVertexAttribPointer(1+i,4,GL_FLOAT,GL_FALSE,sizeof(TwvVitaVertex),
            reinterpret_cast<const void*>(offsetof(TwvVitaVertex,color)+size_t(i)*4*sizeof(float)));
    }
    for (int i=0;i<8;++i) {
        glEnableVertexAttribArray(3+i);
        glVertexAttribPointer(3+i,3,GL_FLOAT,GL_FALSE,sizeof(TwvVitaVertex),
            reinterpret_cast<const void*>(offsetof(TwvVitaVertex,texcoord)+size_t(i)*3*sizeof(float)));
    }
    glEnableVertexAttribArray(11);
    glVertexAttribPointer(11,3,GL_FLOAT,GL_FALSE,sizeof(TwvVitaVertex),
        reinterpret_cast<const void*>(offsetof(TwvVitaVertex,normal)));
    glEnableVertexAttribArray(12);
    glVertexAttribPointer(12,1,GL_FLOAT,GL_FALSE,sizeof(TwvVitaVertex),
        reinterpret_cast<const void*>(offsetof(TwvVitaVertex,pn_index)));
    activeLayout={}; // Force an exact enabled-attribute/layout install on first draw.
    twv::vita::telemetryText("gfx_upload_policy","compact live attributes; borrowed mapped VBOs; native layout cache; exact pipeline index");
    glFrontFace(GL_CW);
    // vitaGL's unpack API supports row length, not GL_UNPACK_ALIGNMENT.
    // Every upload here is tightly packed RGBA8, already four-byte aligned.
    glClearColor(0,0,0,1);
    twv::vita::telemetryText("gfx_init_phase", "initial clear enter");
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    twv::vita::telemetryText("gfx_init_phase", "initial clear return");
    // Optional private generated-source bundle; temporary shaders are released
    // individually. Startup work is reported, never charged as free gameplay.
    twv::vgl::prepare_shader_cache("ux0:data/twilight-vita/shader_warmup.bin",prepare_compile,prepare_release);
    twv::vgl::begin_shader_source_capture("ux0:data/twilight-vita/shader_sources");
    return 1;
}
// Pinned vitaGL has no public global teardown API. The backend releases its
// programs/textures separately; process-lifetime driver pools end with the app.
extern "C" void twv_vgl_shutdown() {
    vertexWriteClassCache.clear();++vertexWriteOwner;
    submit_barrier();
    twv_vgl_texture_conversion_cache_clear();
    if(!twv_vgl_resident_shutdown())twv::vita::telemetryText("gfx_bundle_failure","resident arena shutdown incomplete; mapping retained");
    rasterShadow.invalidate();
    textureSamplerCache.clear();
    programUniformCache.clear();
    if (ready) {
        glFinish();
        release_unit_samplers();
        // Borrowed ring spans must never be freed by glDeleteBuffers.
        glBindBuffer(GL_ARRAY_BUFFER,vertexBuffer); vglBufferData(GL_ARRAY_BUFFER,nullptr);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indexBuffer); vglBufferData(GL_ELEMENT_ARRAY_BUFFER,nullptr);
        glDeleteBuffers(1,&vertexBuffer); glDeleteBuffers(1,&indexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER,0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    }
    // Also clears pending requests after an unsuccessful/partial init.
    release_unit_samplers();
    vertexBuffer=indexBuffer=0; ready = false;
}
extern "C" void twv_vgl_begin() {
    if (!ready) return;
    submit_barrier();
    // GXM display-buffer rotation and clear state are outside draw submission.
    rasterShadow.invalidate();
    // Establish the physical pillarbox, including newly rotated display buffers.
    // backend.cpp then applies Aurora's original per-frame EFB clear values.
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0,0,0,1);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
extern "C" void twv_vgl_end() {
    if (!ready) return;
    submit_barrier();
    const auto error = glGetError();
    if (error != GL_NO_ERROR) {
        ++errors;
        if (errors <= 8) twv::vita::telemetryEvent("gfx_gl_error", error);
    }
    ++frames;
    capture_frame();
    const auto presentStart=twv::vita::processTimeUs();
    vglSwapBuffers(GL_FALSE);
    profileUs[TWV_PROFILE_PRESENT]+=twv::vita::processTimeUs()-presentStart;
    profile_receipts(twv::vita::processTimeUs());
    twv::vita::system_probe_sample(sceKernelGetThreadId(),twv::vita::audio::threadId());
    twv::vita::runControlFrame();
    if (frames % 60 == 0) {
        uint64_t vertexBytes=0,fragmentBytes=0,vertexCalls=0,fragmentCalls=0,failures=0;
        vglGetUniformReservationStats(&vertexBytes,&fragmentBytes,&vertexCalls,&fragmentCalls,&failures);
        twv::vita::telemetryEvent("gfx_native_uniform_vertex_bytes",vertexBytes);
        twv::vita::telemetryEvent("gfx_native_uniform_fragment_bytes",fragmentBytes);
        twv::vita::telemetryEvent("gfx_native_uniform_vertex_calls",vertexCalls);
        twv::vita::telemetryEvent("gfx_native_uniform_fragment_calls",fragmentCalls);
        twv::vita::telemetryEvent("gfx_native_uniform_failures",failures);
        uint64_t nulls=0,negative=0,emptyVertex=0,emptyFragment=0;
        uint32_t firstStage=0,firstBytes=0,lastStage=0,lastBytes=0;
        int32_t firstResult=0,lastResult=0;
        vglGetUniformReservationDiagnostics(&nulls,&negative,&emptyVertex,&emptyFragment,
            &firstStage,&firstResult,&firstBytes,&lastStage,&lastResult,&lastBytes);
        twv::vita::telemetryEvent("gfx_native_uniform_null_successes",nulls);
        twv::vita::telemetryEvent("gfx_native_uniform_negative_errors",negative);
        twv::vita::telemetryEvent("gfx_native_uniform_empty_vertex",emptyVertex);
        twv::vita::telemetryEvent("gfx_native_uniform_empty_fragment",emptyFragment);
        twv::vita::telemetryEvent("gfx_native_uniform_first_stage",firstStage);
        twv::vita::telemetryEvent("gfx_native_uniform_first_result",firstResult);
        twv::vita::telemetryEvent("gfx_native_uniform_first_bytes",firstBytes);
        twv::vita::telemetryEvent("gfx_native_uniform_last_stage",lastStage);
        twv::vita::telemetryEvent("gfx_native_uniform_last_result",lastResult);
        twv::vita::telemetryEvent("gfx_native_uniform_last_bytes",lastBytes);
        const auto log=twv::vita::telemetryStats();
        twv::vita::telemetryEvent("diag_log_written_bytes",log.writtenBytes);
        twv::vita::telemetryEvent("diag_log_write_us",log.writeUs);
        twv::vita::telemetryEvent("diag_log_max_write_us",log.maxWriteUs);
        twv::vita::telemetryEvent("diag_log_writes",log.writes);
        twv::vita::telemetryEvent("diag_log_write_failures",log.writeFailures);
        twv::vita::telemetryEvent("diag_log_overflow_flushes",log.overflowFlushes);
        twv::vita::telemetryEvent("diag_log_lost",log.lost);
        twv::vita::telemetryEvent("diag_log_async",log.async);
        twv::vita::telemetryEvent("diag_log_async_queued_bytes",log.asyncQueuedBytes);
        twv::vita::telemetryEvent("diag_log_async_waits",log.asyncWaits);
        twv::vita::telemetryEvent("diag_log_async_lost_bytes",log.asyncLostBytes);
        twv_resident_geometry_stats resident{};twv_vgl_resident_get_stats(&resident);
        twv::vita::telemetryEvent("gfx_resident_arena_live_bytes",resident.liveBytes);
        twv::vita::telemetryEvent("gfx_resident_arena_retired_bytes",resident.retiredBytes);
        twv::vita::telemetryEvent("gfx_resident_arena_peak_bytes",resident.peakBytes);
        twv::vita::telemetryEvent("gfx_resident_arena_allocations",resident.allocations);
        twv::vita::telemetryEvent("gfx_resident_arena_fallbacks",resident.fallbacks);
        twv::vita::telemetryEvent("gfx_resident_arena_collections",resident.collections);
        twv::vita::telemetryEvent("gfx_resident_arena_completion_failures",resident.completionFailures);
        twv::vita::telemetryEvent("gfx_resident_arena_scanned_units",resident.scannedUnits);
        twv::vita::telemetryEvent("gfx_resident_arena_completed_waits",resident.completedWaits);
        twv::vita::telemetryEvent("gfx_resident_arena_wait_us",resident.waitTimeUs);
        twv::vita::telemetryEvent("gfx_texture_zero_init_bytes",textureZeroInitBytes);
        twv::vita::telemetryEvent("gfx_texture_direct_init_bytes",textureDirectInitBytes);
        if(submitObserverEnabled){
            const auto& s=submitObserver.stats();
            twv::vita::telemetryEvent("gfx_submit_observed_draws",s.draws);
            twv::vita::telemetryEvent("gfx_submit_compatible_pairs",s.compatible_pairs);
            twv::vita::telemetryEvent("gfx_submit_array_proposals",s.array_proposal_pairs);
            twv::vita::telemetryEvent("gfx_submit_longest_run",s.longest_run);
        }
        twv::vgl::shader_compile_report();
        twv::vita::telemetryEvent("gfx_frames_presented", frames);
        twv::vita::telemetryEvent("gfx_draws", draws);
        twv::vita::telemetryEvent("gfx_errors", errors);
        memory_receipts();
        twv::vita::telemetryFlush();
    }
}
extern "C" uint64_t twv_vgl_time_us() { return twv::vita::processTimeUs(); }
extern "C" void twv_vgl_profile_add(uint32_t category,uint64_t microseconds) {
    if (category<TWV_PROFILE_COUNT) profileUs[category]+=microseconds;
}
extern "C" void* twv_vgl_alloc(size_t size) {
    static_assert(alignof(TwvVitaVertex) <= twv::vgl::ScratchAlignment);
    if (!ready) return nullptr;
    auto* result=twv::vgl::aligned_scratch(size, [](size_t bytes) { return vglAllocFromScratch(bytes); });
    static uint32_t sampledFrame=UINT32_MAX;
    if(result && frames<6 && sampledFrame!=frames){
        sampledFrame=frames;
        twv::vita::telemetryEvent("gfx_scratch_sample_type",vgl_mem_get_type_by_addr(result));
    }
    return result;
}
extern "C" void twv_vgl_submit_barrier(void){submit_barrier();}
extern "C" uint32_t twv_vgl_texture_cache_budget(uint32_t current) {
    const auto heap=cpu_heap_usage();
    constexpr uint64_t reserve=48u*1024u*1024u;
    constexpr uint32_t metadata=sizeof(twv::vgl::TextureConversionCache);
    if(!heap.valid || heap.used>_newlib_heap_size_user || current>twv::vgl::TextureConversionCache::limit)return 0;
    const uint64_t reclaimable=current>metadata?current-metadata:0;
    const uint64_t available=uint64_t(_newlib_heap_size_user)-heap.used+reclaimable;
    if(available<=reserve)return 0;
    return uint32_t(std::min<uint64_t>(twv::vgl::TextureConversionCache::limit,available-reserve+metadata));
}
extern "C" uint32_t twv_vgl_geometry_cache_budget(uint32_t current_payload) {
    const auto heap=cpu_heap_usage();
    return twv::vgl::geometry_cache_budget(current_payload,heap.used,_newlib_heap_size_user,heap.valid);
}
extern "C" uint32_t twv_vgl_vertex_is_vram(const void* address) {
    return vgl_mem_get_type_by_addr(const_cast<void*>(address))==VGL_MEM_VRAM;
}
extern "C" uint32_t twv_vgl_vertex_write_class(const void* address,size_t bytes) {
    using twv::vgl::VertexWriteClass;
    const auto result=[](VertexWriteClass value){return uint32_t(value);};
    const uintptr_t ptr=reinterpret_cast<uintptr_t>(address);
    if(!ready || !ptr || !bytes || bytes>UINT32_MAX || bytes>UINTPTR_MAX-ptr)
        return result(VertexWriteClass::Unknown);
    const auto pool=vgl_mem_get_type_by_addr(const_cast<void*>(address));
    // Preserve legacy VRAM staging; do not use this hint as proof of GXM mapping.
    if(pool==VGL_MEM_VRAM)return result(VertexWriteClass::Vram);
    if(pool!=VGL_MEM_RAM)return result(VertexWriteClass::CachedOrExternal);
    if(vertexWriteClassCache.contains(ptr,bytes,vertexWriteOwner)) {
        ++vertexWriteHits;return result(VertexWriteClass::VerifiedUncachedRam);
    }
    SceKernelMemBlockInfo info{};info.size=sizeof info;
    ++vertexWriteQueries;
    const int rc=sceKernelGetMemBlockInfoByRange(const_cast<void*>(address),SceSize(bytes),&info);
    if(rc!=0){++vertexWriteErrors;vertexWriteLastError=rc;return result(VertexWriteClass::Unknown);}
    const uintptr_t base=reinterpret_cast<uintptr_t>(info.mappedBase);
    const bool range=twv::vgl::write_range_contains(base,info.mappedSize,ptr,bytes);
    // Pinned mem_utils.c creates exactly one RAM memblock and retains it for
    // process lifetime; only PHYCONT_ON_DEMAND frees individual native blocks.
    // Confirm BOTH native block endpoints belong to that RAM pool before caching
    // its full range. Never cache arbitrary external/overflow allocations.
    const bool persistent=range &&
        vgl_mem_get_type_by_addr(info.mappedBase)==VGL_MEM_RAM &&
        vgl_mem_get_type_by_addr(reinterpret_cast<void*>(base+info.mappedSize-1))==VGL_MEM_RAM;
    const bool uncached=info.type==SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    const bool writable=(info.access&SCE_KERNEL_MEMORY_ACCESS_W)!=0;
    if(vertexWriteClassCache.remember(ptr,bytes,base,info.mappedSize,rc,uncached,writable,persistent,vertexWriteOwner))
        return result(VertexWriteClass::VerifiedUncachedRam);
    return result(VertexWriteClass::Unknown);
}
extern "C" void twv_vgl_vertex_write_class_report(void) {
    twv::vita::telemetryEvent("gfx_vertex_write_class_queries",vertexWriteQueries);
    twv::vita::telemetryEvent("gfx_vertex_write_class_cache_hits",vertexWriteHits);
    twv::vita::telemetryEvent("gfx_vertex_write_class_errors",vertexWriteErrors);
    twv::vita::telemetryEvent("gfx_vertex_write_class_last_error",vertexWriteLastError);
}
extern "C" uint32_t twv_vgl_program(const char* vs, const char* fs) {
    submit_barrier();
    ProgramKey key{vs,fs};
    if (const auto found=programCache.find(key);found!=programCache.end()) {
        ++programs[found->second].references; ++programCacheHits; return found->second;
    }
    twv::vita::telemetryText("gfx_program_phase", "compile vertex");
    const GLuint vertex = acquire_shader(GL_CG_VERTEX_SHADER_EXT, vs);
    twv::vita::telemetryText("gfx_program_phase", "compile fragment");
    const GLuint fragment = acquire_shader(GL_CG_FRAGMENT_SHADER_EXT, fs);
    if (!vertex || !fragment) {
        release_shader(vertex);release_shader(fragment);
        return 0;
    }
    const GLuint program = glCreateProgram();
    if(!program) {
        release_shader(vertex);release_shader(fragment);
        twv::vita::telemetryText("shader_link_failure","glCreateProgram returned zero");
        return 0;
    }
    twv::vita::telemetryEvent("gfx_program_link_enter", program);
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "color0");
    glBindAttribLocation(program, 2, "color1");
    glBindAttribLocation(program,11,"normal");
    glBindAttribLocation(program,12,"pn_index");
    for (int i=0; i<8; ++i) {
        char name[32]; std::snprintf(name,sizeof name,"tc%d",i);
        glBindAttribLocation(program, 3+i, name);
    }
    glLinkProgram(program);
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char error[2048]{};
        glGetProgramInfoLog(program, sizeof error, nullptr, error);
        twv::vita::telemetryText("shader_link_failure", error);
        glDeleteProgram(program);
        release_shader(vertex);release_shader(fragment);
        return 0;
    }
    twv::vita::telemetryEvent("gfx_program_link_return", program);
    if (programs.size()<=program) programs.resize(program+1);
    programUniformCache.invalidate(program); // Native object IDs can be reused.
    auto& info=programs[program]; info={}; info.references=1; info.key=key;
    info.stages={vertex,fragment};
    const char* names[]={"proj_rows","regs","konst","fog"};
    for(int i=0;i<4;++i) info.uniforms[i]=glGetUniformLocation(program,names[i]);
    const char* vertexNames[]={"pn_pos","pn_nrm","light_pos","light_dir","light_color",
        "light_cos","light_dist","channel_mat","channel_amb","light_masks","texgen_rows"};
    for(int i=0;i<11;++i) info.vertexUniforms[i]=glGetUniformLocation(program,vertexNames[i]);
    glUseProgram(program); activeProgram=program;
    for(int i=0;i<8;++i) {
        char name[16]; std::snprintf(name,sizeof name,"tex%d",i);
        info.samplers[i]=glGetUniformLocation(program,name);
        if(info.samplers[i]>=0) glUniform1i(info.samplers[i],i);
    }
    programCache.emplace(std::move(key),program); ++uniquePrograms;
    return program;
}
extern "C" void twv_vgl_program_delete(uint32_t program) {
    submit_barrier();
    if (!program || program>=programs.size() || !programs[program].references) return;
    auto& info=programs[program];
    if(--info.references) return;
    programUniformCache.invalidate(program);
    programCache.erase(info.key); info.key={};
    if(activeProgram==program) {glUseProgram(0);activeProgram=0;}
    glDeleteProgram(program);
    release_shader(info.stages[0]);release_shader(info.stages[1]);info.stages={};
}
extern "C" void twv_vgl_draw(const TwvVitaDrawState* s, const void* v,
                                uint32_t count, const uint16_t* indices, uint32_t index_count) {
    const auto samplerMask=consume_unit_sampler_requests();
    if (!ready || !s || !s->program || !v || s->program>=programs.size()) return;
    if(submitObserverEnabled)submitObserver.observe(*s,v,count,indices,index_count);
    const auto& program=programs[s->program];
    record_active_sampler_aliases(program,s,samplerMask);
    submit_raster(s);
    if(activeProgram!=s->program) { glUseProgram(s->program); activeProgram=s->program; }
    submit_uniforms(program,s);
    for (int i=0; i<8; ++i) if(program.samplers[i]>=0) {
        glActiveTexture(GL_TEXTURE0+i);
        glBindTexture(GL_TEXTURE_2D,s->textures[i]);
        // glUniform1i associates texN with logical unitN. GXM resource slots
        // can be compacted, so never use their order as the sampler unit.
        bind_draw_sampler(unsigned(i),samplerMask);
    }
    if(std::memcmp(&activeLayout,&s->vertex_layout,sizeof activeLayout)!=0) {
        const auto& layout=s->vertex_layout;
        for(unsigned attr=0;attr<TwvVertexAttributeCount;++attr) {
            if(layout.offsets[attr]==TwvVertexAttributeAbsent) glDisableVertexAttribArray(attr);
            else {
                glEnableVertexAttribArray(attr);
                glVertexAttribPointer(attr,layout.components[attr],GL_FLOAT,GL_FALSE,layout.stride,
                    reinterpret_cast<const void*>(uintptr_t(layout.offsets[attr])));
            }
        }
        activeLayout=layout;
    }
    vglBufferData(GL_ARRAY_BUFFER,v);
    directVertexBytes+=uint64_t(count)*s->vertex_layout.stride;
    if (index_count) {
        vglBufferData(GL_ELEMENT_ARRAY_BUFFER,indices);
        directIndexBytes+=uint64_t(index_count)*sizeof(*indices);
        glDrawElements(GL_TRIANGLES,index_count,GL_UNSIGNED_SHORT,nullptr);
    }
    else glDrawArrays(GL_TRIANGLES,0,count);
    ++draws;
}
extern "C" uint32_t twv_vgl_texture(uint32_t width, uint32_t height, uint32_t levels) {
    submit_barrier();
    GLint maximum=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
    if (!width || !height || width>uint32_t(maximum) || height>uint32_t(maximum)) {
        twv::vita::telemetryText("texture_dimension_failure","texture exceeds actual GPU limit");
        return 0;
    }
    GLuint id = 0; glGenTextures(1,&id); glBindTexture(GL_TEXTURE_2D,id);
    textureSamplerCache.invalidate(id);
    // Unsupported EFB conversion targets must never sample uninitialized VRAM.
    // Their missing operations remain explicitly counted by backend.cpp.
    std::vector<unsigned char> zero(size_t(width)*height*4,0);
    for (uint32_t level=0; level<levels; ++level) {
        textureZeroInitBytes+=uint64_t(width)*height*4;
        glTexImage2D(GL_TEXTURE_2D,level,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,zero.data());
        width = width>1 ? width/2 : 1; height = height>1 ? height/2 : 1;
    }
    // Pinned vitaGL generates lower mips asynchronously even for zero data.
    // Callers may immediately replace them using CPU glTexSubImage2D copies.
    if(levels>1) {
        const int transferResult=sceGxmTransferFinish();
        if(transferResult<0) {
            char error[96];std::snprintf(error,sizeof error,"zero texture mip transfer finish failed: %d",transferResult);
            twv::vita::telemetryText("gfx_bundle_failure",error);
            std::abort(); // Completion unknown: neither publish ID nor free its backing.
        }
    }
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    return id;
}
extern "C" uint32_t twv_vgl_texture_static_rgba(uint32_t width,uint32_t height,uint32_t levels,const void* bytes,size_t size) {
    if(!twv::vgl::complete_rgba_chain(width,height,levels,bytes,size))return 0;
    GLint maximum=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
    if(maximum<=0 || width>uint32_t(maximum) || height>uint32_t(maximum))return 0;
    submit_barrier();
    GLuint id=0;glGenTextures(1,&id);if(!id)return 0;
    glBindTexture(GL_TEXTURE_2D,id);textureSamplerCache.invalidate(id);
    const uint32_t baseWidth=width,baseHeight=height;
    const auto* data=static_cast<const unsigned char*>(bytes);size_t offset=0;
    for(uint32_t level=0;level<levels;++level){
        const size_t count=size_t(width)*height*4;
        glTexImage2D(GL_TEXTURE_2D,level,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,data+offset);
        offset+=count;width=width>1?width/2:1;height=height>1?height/2:1;
    }
    // Pinned vitaGL ignores uncompressed glTexImage2D data for levels > 0:
    // gpu_alloc_mipmaps regenerates the chain from level 0 on each definition.
    // Complete all definitions, then wait for its asynchronous transfer writes
    // before replacing lower levels with the original GX mip bytes.
    if(levels>1) {
        const int transferResult=sceGxmTransferFinish();
        if(transferResult<0) {
            char error[96];std::snprintf(error,sizeof error,"static texture mip transfer finish failed: %d",transferResult);
            twv::vita::telemetryText("gfx_bundle_failure",error);
            std::abort(); // Existing wrapped abort flushes receipts before stopping.
        }
        size_t mipOffset=size_t(baseWidth)*baseHeight*4;
        width=baseWidth;height=baseHeight;
        for(uint32_t level=1;level<levels;++level) {
            width=width>1?width/2:1;height=height>1?height/2:1;
            glTexSubImage2D(GL_TEXTURE_2D,level,0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,data+mipOffset);
            mipOffset+=size_t(width)*height*4;
        }
    }
    textureDirectInitBytes+=offset;
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    return id;
}
extern "C" void twv_vgl_texture_write(uint32_t texture,uint32_t level,uint32_t w,uint32_t h,const void* rgba) {
    submit_barrier();
    // Conservatively reapply after uploads in case the native mip metadata
    // changed. The cache does not infer which driver texture writes are inert.
    textureSamplerCache.invalidate(texture);
    glBindTexture(GL_TEXTURE_2D,texture);
    glTexSubImage2D(GL_TEXTURE_2D,level,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
}
extern "C" void twv_vgl_texture_delete(uint32_t texture) {
    submit_barrier();
    textureSamplerCache.invalidate(texture);
    if(texture)glDeleteTextures(1,&texture);
}
extern "C" void twv_vgl_texture_sampler(uint32_t texture,int min,int mag,int s,int t) {
    ++samplerRequests;
#if TWV_VITA_SAMPLER_CACHE
    const twv::vgl::SamplerParams params{min,mag,s,t};
    if(textureSamplerCache.unchanged(texture,params)){++samplerSkips;return;}
#endif
    submit_barrier();
    const GLenum filters[] = {GL_NEAREST,GL_LINEAR,GL_NEAREST_MIPMAP_NEAREST,
        GL_LINEAR_MIPMAP_NEAREST,GL_NEAREST_MIPMAP_LINEAR,GL_LINEAR_MIPMAP_LINEAR};
    glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,min>=0&&min<6?filters[min]:GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,mag ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap(s));
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap(t));
    ++samplerUpdates;
#if TWV_VITA_SAMPLER_CACHE
    textureSamplerCache.applied(texture,params);
#endif
}
extern "C" void twv_vgl_clear(float r,float g,float b,float a,float z,unsigned mask,int x,int y,int w,int h) {
    submit_barrier();
    rasterShadow.invalidate();
    glEnable(GL_SCISSOR_TEST); glScissor(x,y,w,h);
    glColorMask(mask&1,mask&1,mask&1,mask&2);
    glDepthMask((mask&4) ? GL_TRUE : GL_FALSE);
    glClearColor(r,g,b,a); glClearDepthf(z);
    glClear(((mask&3) ? GL_COLOR_BUFFER_BIT : 0) | ((mask&4) ? GL_DEPTH_BUFFER_BIT : 0));
}
