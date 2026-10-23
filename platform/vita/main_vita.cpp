// Vita process entry. Validates port options and records clock policy before
// handing control to Dusklight's unmodified PC entry point
// (src/m_Do/m_Do_main.cpp game_main) with the disc at its fixed Vita location.
#include "runtime/telemetry.hpp"
#include "runtime/renderer_options.hpp"
#include "runtime/gpu_options.hpp"
#include "runtime/clock_policy.hpp"
#include "runtime/run_control.hpp"
#include "runtime/run_sequence.hpp"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

int game_main(int argc, char* argv[]); // Dusklight, built with int-sized enums; plain types only
// Plain integer C ABI across SDK short-enum / game int-enum domains.
extern "C" int twv_vita_stream_decode_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_neon_compare_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_shadow_image_skip_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_vertex_cull_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_sparse_spans_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_small_packets_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_uncached_staging_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_gx_fast_select(int enabled) noexcept __attribute__((weak));
extern "C" int twv_vita_lazy_vertex_plan_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_static_geometry_select(int mode) __attribute__((weak));
// T2 (patches/aurora/0007): FIFO processing on its own worker; the game thread then keeps core 0.
extern "C" int twv_vita_gx_thread_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_shader_capture_select(int enabled) __attribute__((weak));
extern "C" int twv_vita_neon_decode_select(int mode) __attribute__((weak));
extern "C" int twv_vita_pipeline_fast_select(int mode) __attribute__((weak));
// Strong on purpose: it pulls runtime/gx_thread.o (and the worker's core hook) into the link.
extern "C" int twv_vita_gx_thread_pin_main(void);
// G0/G1 (gfx_vgl/gpu_dispatch.cpp): backend, draw digest and frame capture. Absent in null-graphics builds.
extern "C" int twv_vita_gpu_select(int backend, int digest, uint32_t digest_first, uint32_t digest_last,
                                   uint32_t capture_first, uint32_t capture_last, uint32_t capture_step,
                                   int capture_match, uint32_t features, uint32_t split_scenes) __attribute__((weak));
#if TWV_VITA_BENCHMARK
extern "C" int twv_vita_benchmark_enable(const char*);
#endif

// Explicit CPU allocation budget; renderer allocations stay outside newlib.
// MEM1/ARAM reserves are bounded independently in the Aurora adapter.
extern "C" {
unsigned int _newlib_heap_size_user = 160 * 1024 * 1024;
}

namespace {
// SDK output-buffer ABI (from a data abort on a Vita 1000): arm-vita-eabi uses short enums,
// so out-parameters are structs of the pinned size or word-sized buffers only.
static_assert(sizeof(SceKernelFreeMemorySizeInfo) == 0x10, "sceKernelGetFreeMemorySize output");

void logFreeMemory(const char* phase) {
    SceKernelFreeMemorySizeInfo info{};
    info.size = sizeof info;
    if (sceKernelGetFreeMemorySize(&info) < 0) return;
    twv::vita::telemetryText("memory_phase", phase);
    twv::vita::telemetryEvent("free_user_kib", (long long)(info.size_user / 1024));
    twv::vita::telemetryEvent("free_cdram_kib", (long long)(info.size_cdram / 1024));
    twv::vita::telemetryEvent("free_phycont_kib", (long long)(info.size_phycont / 1024));
}

void selectClocks(const twv::vita::ClockOptions& options) {
    using namespace twv::vita;
    const ClockNative native{
        scePowerGetArmClockFrequency, scePowerGetBusClockFrequency,
        scePowerGetGpuClockFrequency, scePowerGetGpuXbarClockFrequency,
        scePowerSetArmClockFrequency, scePowerSetBusClockFrequency,
        scePowerSetGpuClockFrequency, scePowerSetGpuXbarClockFrequency,
    };
    const auto receipt = applyClockPolicy(options.mode, native);
    telemetryText("clock_mode", clockModeName(options.mode));
    telemetryEvent("clock_mode_explicit", options.selected);
    telemetryEvent("clock_setter_attempts", receipt.settersAttempted ? 4 : 0);
    const char* names[]{"arm", "bus", "gpu", "gpu_xbar"};
    const int initial[]{receipt.initial.arm, receipt.initial.bus, receipt.initial.gpu, receipt.initial.gpuXbar};
    const int observed[]{receipt.observed.arm, receipt.observed.bus, receipt.observed.gpu, receipt.observed.gpuXbar};
    const int requested[]{Legacy444Requests.arm, Legacy444Requests.bus, Legacy444Requests.gpu, Legacy444Requests.gpuXbar};
    const int results[]{receipt.setterResults.arm, receipt.setterResults.bus, receipt.setterResults.gpu, receipt.setterResults.gpuXbar};
    for (unsigned i = 0; i < 4; ++i) {
        char event[64];
        std::snprintf(event, sizeof event, "clock_initial_%s_mhz_result", names[i]);
        telemetryEvent(event, initial[i]);
        if (receipt.settersAttempted) {
            std::snprintf(event, sizeof event, "clock_set_%s_requested_mhz", names[i]);
            telemetryEvent(event, requested[i]);
            std::snprintf(event, sizeof event, "clock_set_%s_result", names[i]);
            telemetryEvent(event, results[i]);
        }
        std::snprintf(event, sizeof event, "clock_observed_%s_mhz_result", names[i]);
        telemetryEvent(event, observed[i]);
    }
    // Retain existing boot fields as aliases of the signed observed readings.
    telemetryEvent("arm_mhz", receipt.observed.arm);
    telemetryEvent("gpu_mhz", receipt.observed.gpu);
}
}

int main(int, char**) {
    twv::vita::telemetryInit();
    twv::vita::installFaultReceipts();
    logFreeMemory("boot");

    // User data (config, saves) lives in the Vita data folder via Borealis' standard
    // --user-dir option; Dusklight's cache and logs stay in SDL's preference path
    // (ux0:data/TwilitRealm/Dusklight on the Vita), as upstream chooses it.
    static char arg0[] = "twilight-vita";
    static char arg1[] = "--user-dir";
    static char arg2[128];
    static char arg3[] = "--dvd";
    static char arg4[128];
    std::snprintf(arg2, sizeof arg2, "%s", twv::vita::DataRoot);
    std::snprintf(arg4, sizeof arg4, "%s", twv::vita::DefaultDisc);
    twv::vita::telemetryText("user_dir", arg2);
    twv::vita::telemetryText("disc_path", arg4);

    struct stat discInfo{};
    if (stat(arg4, &discInfo) != 0 || !S_ISREG(discInfo.st_mode)) {
        twv::vita::telemetryText("no_disc", "Supply your own game.iso in ux0:data/twilight-vita/");
        twv::vita::telemetryFlush();
        sceKernelExitProcess(2);
        return 2;
    }
    // Optional native test entry; no actor/camera placement or fabricated save.
    // Original game_main owns option validation and original stage/spawn loading.
    static char extra[512];
    char* gameArgs[32] = {arg0, arg1, arg2, arg3, arg4};
    int gameArgc = 5;
    twv::vita::RendererOptions rendererOptions;
    twv::vita::GpuOptions gpuOptions;
    twv::vita::ClockOptions clockOptions;
    twv::vita::ExitAfterOption exitAfter;
    // A test sequence (run_sequence.hpp) takes precedence over args.txt until
    // its last run has been launched.
    static char sequenceText[twv::vita::SequenceFileLimit + 1];
    bool haveArgs = false;
    if (FILE* file = std::fopen(twv::vita::SequenceFile, "rb")) {
        const auto n = std::fread(sequenceText, 1, sizeof sequenceText - 1, file);
        const bool tooLong = n == sizeof sequenceText - 1 && std::fgetc(file) != EOF;
        std::fclose(file);
        if (tooLong) {
            twv::vita::telemetryText("args_error", "sequence.txt exceeds 4096 bytes");
            twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
        }
        sequenceText[n] = 0;
        char state[32] = {};
        if (FILE* stateFile = std::fopen(twv::vita::SequenceStateFile, "rb")) {
            state[std::fread(state, 1, sizeof state - 1, stateFile)] = 0;
            std::fclose(stateFile);
        }
        const unsigned index = twv::vita::parse_sequence_state(state);
        twv::vita::SequenceRun run;
        if (twv::vita::select_sequence_run(sequenceText, index, run)) {
            if (std::strlen(run.args) >= sizeof extra) {
                twv::vita::telemetryText("args_error", "sequence run exceeds 511 bytes");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
            }
            std::snprintf(extra, sizeof extra, "%s", run.args);
            haveArgs = true;
            // Advance before starting, so a crash moves on at the next launch.
            bool saved = false;
            if (FILE* stateFile = std::fopen(twv::vita::SequenceStateFile, "wb")) {
                saved = std::fprintf(stateFile, "%u\n", index + 1) > 0;
                saved = std::fclose(stateFile) == 0 && saved;
            }
            twv::vita::telemetryText("sequence_label", run.label);
            twv::vita::telemetryEvent("sequence_index", index);
            twv::vita::telemetryEvent("sequence_count", run.count);
            twv::vita::telemetryEvent("sequence_state_saved", saved);
        } else {
            twv::vita::telemetryEvent("sequence_complete", run.count);
        }
    }
    if (!haveArgs) {
        if (FILE* file = std::fopen("ux0:data/twilight-vita/args.txt", "rb")) {
            const auto n = std::fread(extra, 1, sizeof extra - 1, file);
            const bool tooLong = n == sizeof extra - 1 && std::fgetc(file) != EOF;
            std::fclose(file);
            if (tooLong) {
                twv::vita::telemetryText("args_error", "args.txt exceeds 511 bytes");
                sceKernelExitProcess(2);
                return 2;
            }
            extra[n] = 0;
            haveArgs = true;
        }
    }
    if (haveArgs) {
        for (char* token = std::strtok(extra, " \t\r\n"); token; token = std::strtok(nullptr, " \t\r\n")) {
            const int clockOption = clockOptions.consume(token);
            if (clockOption < 0) {
                twv::vita::telemetryText("args_error", "clock mode invalid or selected more than once; use --vita-clocks=system|legacy444");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
            }
            if (clockOption > 0) continue;
            const int exitOption = exitAfter.consume(token);
            if (exitOption < 0) {
                twv::vita::telemetryText("args_error", "--vita-exit-after needs 10..3600 seconds, once");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
            }
            if (exitOption > 0) continue;
            const int gpuOption = gpuOptions.consume(token);
            if (gpuOption < 0) {
                twv::vita::telemetryText("args_error", "GPU option invalid or selected more than once; see runtime/gpu_options.hpp");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
            }
            if (gpuOption > 0) {
                twv::vita::telemetryText("gpu_option", token);
                continue;
            }
            const int rendererOption=rendererOptions.consume(token);
            const bool unavailable=(rendererOption==1 && (!twv_vita_stream_decode_select ||
                    !twv_vita_stream_decode_select(rendererOptions.streamDecode))) ||
                (rendererOption==2 && (!twv_vita_neon_compare_select ||
                    !twv_vita_neon_compare_select(rendererOptions.neonCompare))) ||
                (rendererOption==3 && (!twv_vita_shadow_image_skip_select ||
                    !twv_vita_shadow_image_skip_select(rendererOptions.shadowImageSkip))) ||
                (rendererOption==4 && (!twv_vita_vertex_cull_select ||
                    !twv_vita_vertex_cull_select(rendererOptions.vertexCull))) ||
                (rendererOption==5 && (!twv_vita_sparse_spans_select ||
                    !twv_vita_sparse_spans_select(rendererOptions.sparseSpans))) ||
                (rendererOption==12 && (!twv_vita_small_packets_select ||
                    !twv_vita_small_packets_select(rendererOptions.smallPackets))) ||
                (rendererOption==13 && (!twv_vita_uncached_staging_select ||
                    !twv_vita_uncached_staging_select(rendererOptions.uncachedStaging))) ||
                (rendererOption==14 && (!twv_vita_gx_fast_select ||
                    !twv_vita_gx_fast_select(rendererOptions.gxDecode))) ||
                (rendererOption==15 && (!twv_vita_lazy_vertex_plan_select ||
                    !twv_vita_lazy_vertex_plan_select(rendererOptions.lazyVertexPlan))) ||
                (rendererOption==16 && (!twv_vita_static_geometry_select ||
                    !twv_vita_static_geometry_select(rendererOptions.staticGeometry))) ||
                (rendererOption==17 && (!twv_vita_gx_thread_select ||
                    !twv_vita_gx_thread_select(rendererOptions.gxThread))) ||
                (rendererOption==18 && (!twv_vita_shader_capture_select ||
                    !twv_vita_shader_capture_select(rendererOptions.shaderCapture))) ||
                (rendererOption==19 && (!twv_vita_neon_decode_select ||
                    !twv_vita_neon_decode_select(rendererOptions.neonDecode))) ||
                (rendererOption==20 && (!twv_vita_pipeline_fast_select ||
                    !twv_vita_pipeline_fast_select(rendererOptions.pipelineFast)));
            if(rendererOption<0 || unavailable) {
                twv::vita::telemetryText("args_error","renderer option unavailable, invalid or selected more than once");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
            }
            if(rendererOption==1) {
                twv::vita::telemetryText("vertex_stream_requested",rendererOptions.streamDecode?"on":"off");
                continue;
            }
            if(rendererOption==2) {
                twv::vita::telemetryText("geometry_neon_requested",rendererOptions.neonCompare?"on":"off");
                continue;
            }
            if(rendererOption==3) {
                twv::vita::telemetryText("shadow_image_skip_requested",rendererOptions.shadowImageSkip?"on":"off");
                continue;
            }
            if(rendererOption==4) {
                twv::vita::telemetryText("vertex_cull_requested",rendererOptions.vertexCull?"on":"off");
                continue;
            }
            if(rendererOption==5) {
                twv::vita::telemetryText("sparse_spans_requested",rendererOptions.sparseSpans?"on":"off");
                continue;
            }
            if(rendererOption==12 || rendererOption==13) {
                twv::vita::telemetryText(rendererOption==12?"small_packets_requested":"uncached_staging_requested",
                    (rendererOption==12?rendererOptions.smallPackets:rendererOptions.uncachedStaging)?"on":"off");
                continue;
            }
            if(rendererOption==14) {
                twv::vita::telemetryText("gx_decode_requested",rendererOptions.gxDecode?"on":"off");
                continue;
            }
            if(rendererOption==15) {
                twv::vita::telemetryText("lazy_vertex_plan_requested",rendererOptions.lazyVertexPlan?"on":"off");
                continue;
            }
            if(rendererOption==19) {
                const char* names[]={"off","on","verify"};
                twv::vita::telemetryText("neon_decode_requested",names[rendererOptions.neonDecode]);
                continue;
            }
            if(rendererOption==20) {
                const char* names[]={"off","on","verify"};
                twv::vita::telemetryText("pipeline_fast_requested",names[rendererOptions.pipelineFast]);
                continue;
            }
            if(rendererOption==18) {
                twv::vita::telemetryText("shader_capture_requested",rendererOptions.shaderCapture?"on":"off");
                continue;
            }
            if(rendererOption==17) {
                twv::vita::telemetryText("gx_thread_requested",rendererOptions.gxThread?"on":"off");
                continue;
            }
            if(rendererOption==16) {
                const char* names[]={"off","on","verify"};
                twv::vita::telemetryText("static_geometry_requested",names[rendererOptions.staticGeometry]);
                continue;
            }
            if(rendererOption>=6) {
                twv::vita::telemetryText("render_bundle_option",token);
                continue;
            }
            if(std::strncmp(token,"--vita-benchmark=",17)==0) {
#if TWV_VITA_BENCHMARK
                if(twv_vita_benchmark_enable(token+17))continue;
#endif
                twv::vita::telemetryText("args_error","benchmark unavailable, unknown or selected more than once");
                sceKernelExitProcess(2);return 2;
            }
            if (gameArgc == 31) {
                twv::vita::telemetryText("args_error", "too many arguments");
                sceKernelExitProcess(2);
                return 2;
            }
            gameArgs[gameArgc++] = token;
            twv::vita::telemetryText("extra_arg", token);
        }
    }
    gameArgs[gameArgc] = nullptr;
    if(!twv_vita_bundle_select(rendererOptions.bundle_features(),rendererOptions.memoryProfile)) {
        twv::vita::telemetryText("args_error","render bundle unavailable or selected after first use");
        twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
    }
    if (gpuOptions.any()) {
        if (!gpuOptions.consistent() || !twv_vita_gpu_select ||
            !twv_vita_gpu_select(gpuOptions.backend, gpuOptions.digest, gpuOptions.digestFirst,
                                 gpuOptions.digestLast, gpuOptions.captureFirst, gpuOptions.captureLast,
                                 gpuOptions.captureStep, gpuOptions.captureMatch, gpuOptions.features,
                                 gpuOptions.splitScenes)) {
            twv::vita::telemetryText("args_error", "GPU backend not in this build, capture-match without digest, or a GXM feature without --vita-gpu=gxm");
            twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
        }
    }
    twv::vita::telemetryText("gpu_backend_requested", gpuOptions.backend ? "gxm" : "vgl");
    twv::vita::telemetryEvent("render_bundle_compiled",twv_vita_bundle_compiled());
    twv::vita::telemetryEvent("render_bundle_requested_features",rendererOptions.bundle_features());
    twv::vita::telemetryEvent("render_bundle_explicit",rendererOptions.bundleSelected || rendererOptions.featureSelected || rendererOptions.memorySelected);
    twv::vita::telemetryEvent("render_bundle_requested_memory_profile",rendererOptions.memoryProfile);
    if(!rendererOptions.selected)twv::vita::telemetryText("vertex_stream_requested","default off");
    if(!rendererOptions.neonSelected)twv::vita::telemetryText("geometry_neon_requested","default off");
    if(!rendererOptions.shadowSelected)twv::vita::telemetryText("shadow_image_skip_requested","default off");
    if(!rendererOptions.cullSelected)twv::vita::telemetryText("vertex_cull_requested","default off");
    if(!rendererOptions.sparseSelected)twv::vita::telemetryText("sparse_spans_requested","default off");
    if(!rendererOptions.smallSelected)twv::vita::telemetryText("small_packets_requested","default off");
    if(!rendererOptions.uncachedSelected)twv::vita::telemetryText("uncached_staging_requested","default off");
    if(!rendererOptions.gxSelected)twv::vita::telemetryText("gx_decode_requested","default off");
    if(!rendererOptions.lazyPlanSelected)twv::vita::telemetryText("lazy_vertex_plan_requested","default off");
    if(!rendererOptions.staticGeometrySelected)twv::vita::telemetryText("static_geometry_requested","default off");
    if(!rendererOptions.gxThreadSelected)twv::vita::telemetryText("gx_thread_requested","default off");
    if(!rendererOptions.pipelineFastSelected)twv::vita::telemetryText("pipeline_fast_requested","default off");

    // All port-owned options and selection failures have been checked. The
    // original game's CLI validation remains game_main's responsibility.
    selectClocks(clockOptions);
    if (rendererOptions.gxThread)
        twv::vita::telemetryEvent("gx_thread_main_affinity_result", twv_vita_gx_thread_pin_main());
    if (exitAfter.selected) {
        twv::vita::runControlExitAfter(exitAfter.seconds);
        twv::vita::telemetryEvent("run_exit_after_requested_s", exitAfter.seconds);
    }
    twv::vita::telemetryEvent("game_main_enter", 0);
    const int result = game_main(gameArgc, gameArgs);
    twv::vita::telemetryEvent("game_main_exit", result);
    twv::vita::telemetrySync();
    sceKernelExitProcess(result);
    return result;
}
