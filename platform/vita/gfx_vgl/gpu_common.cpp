// SDK domain: receipts shared by the vitaGL and GXM backends (see gpu_common.hpp).
#include "gpu_common.hpp"
#include "cache_budget.hpp"
#include "frame_pacing.hpp"
#include "profile_scope.hpp"
#include "shader_preparation.hpp"
#include "texture_conversion_cache.hpp"
#include "texture_upload_api.h"
#include "runtime/telemetry.hpp"
#include "runtime/system_probe.hpp"
#include "runtime/run_control.hpp"
#include "audio/audio_out.hpp"
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <malloc.h>
extern "C" unsigned int _newlib_heap_size_user;

namespace twv::gpu {
namespace {
std::array<uint64_t,TWV_PROFILE_COUNT> profileUs{};
twv::vgl::FramePacing framePacing;
uint32_t frames=0;

void profile_receipts(const TwvGpuBackend& backend,uint64_t now) {
    twv::vgl::FramePacingStats pacing;
    if(!framePacing.frame_end(now,pacing))return;
    // Free play (telemetryQuiet): one complete 30-frame window in every 300 frames.
    static unsigned windows=0;
    if(twv::vita::telemetryIsQuiet() &&
       ++windows%(twv::vita::QuietReceiptFrames/twv::vgl::FramePacing::Window)) {
        profileUs.fill(0);
        return;
    }
    const char* names[] = {"prof_vertex_cpu_us", "prof_gl_submit_us", "prof_shader_us",
        "prof_state_us", "prof_pipeline_us", "prof_present_us",
        "prof_fifo_inclusive_us", "prof_game_execute_inclusive_us", "prof_game_audio_us",
        "prof_vi_callbacks_us", "prof_painter_inclusive_us", "prof_events_us",
        "prof_tick_execute_inclusive_us", "prof_tick_draw_inclusive_us", "prof_tick_particle3d_us",
        "prof_tick_shadow_collect_us", "prof_tick_demo_us", "prof_tick_shadow_collect_calls_x1000",
        "prof_tick_bg_acch_us", "prof_tick_bg_ground_us", "prof_tick_bg_line_us", "prof_tick_cc_move_us",
        "prof_tick_bg_queries", "prof_gx_wait_us"};
    static_assert(sizeof names/sizeof names[0]==TWV_PROFILE_COUNT);
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
    if(backend.pacing_report)backend.pacing_report();
    const auto audio=twv::vita::audio::stats();
    twv::vita::telemetryEvent("audio_buffers",audio.buffers);
    twv::vita::telemetryEvent("audio_late_buffers",audio.late);
    twv::vita::telemetryEvent("audio_max_render_us",audio.maxRenderUs);
    twv::vita::telemetryFlush();
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
}
CpuHeapUsage cpu_heap_usage() {
    const auto info=mallinfo();
    CpuHeapUsage result;
    if(info.arena>0 && info.uordblks>=0 && info.hblkhd>=0 && info.uordblks<=info.arena) {
        const uint64_t used=uint64_t(info.uordblks)+uint64_t(info.hblkhd);
        if(used<=UINT32_MAX)result={1,uint32_t(used),uint32_t(info.arena)};
    }
    return result;
}
void common_init() {framePacing.reset();profileUs.fill(0);}
uint32_t frames_presented() {return frames;}
void common_frame(const TwvGpuBackend& backend) {
    ++frames;
    profile_receipts(backend,twv::vita::processTimeUs());
    twv::vita::system_probe_sample(sceKernelGetThreadId(),twv::vita::audio::threadId());
    twv::vita::runControlFrame();
    if(frames%(twv::vita::telemetryIsQuiet()?twv::vita::QuietReceiptFrames:60))return;
    if(backend.report)backend.report();
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
    twv::vgl::shader_compile_report();
    twv::vita::telemetryEvent("gfx_frames_presented",frames);
    memory_receipts();
    twv::vita::telemetryFlush();
}
}

extern "C" uint64_t twv_vgl_time_us() { return twv::vita::processTimeUs(); }
extern "C" void twv_vgl_profile_add(uint32_t category,uint64_t microseconds) {
    if (category<TWV_PROFILE_COUNT) twv::gpu::profileUs[category]+=microseconds;
}
extern "C" uint32_t twv_vgl_texture_cache_budget(uint32_t current) {
    const auto heap=twv::gpu::cpu_heap_usage();
    constexpr uint64_t reserve=48u*1024u*1024u;
    constexpr uint32_t metadata=sizeof(twv::vgl::TextureConversionCache);
    if(!heap.valid || heap.used>_newlib_heap_size_user || current>twv::vgl::TextureConversionCache::limit)return 0;
    const uint64_t reclaimable=current>metadata?current-metadata:0;
    const uint64_t available=uint64_t(_newlib_heap_size_user)-heap.used+reclaimable;
    if(available<=reserve)return 0;
    return uint32_t(std::min<uint64_t>(twv::vgl::TextureConversionCache::limit,available-reserve+metadata));
}
extern "C" uint32_t twv_vgl_geometry_cache_budget(uint32_t current_payload) {
    const auto heap=twv::gpu::cpu_heap_usage();
    return twv::vgl::geometry_cache_budget(current_payload,heap.used,_newlib_heap_size_user,heap.valid);
}
