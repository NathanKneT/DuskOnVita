// Vita process entry. Validates port options and records clock policy before
// handing control to Dusklight's unmodified PC entry point
// (src/m_Do/m_Do_main.cpp game_main) with the disc at its fixed Vita location.
#include "runtime/telemetry.hpp"
#include "runtime/renderer_options.hpp"
#include "runtime/gpu_options.hpp"
#include "runtime/clock_policy.hpp"
#include "runtime/run_control.hpp"
#include "runtime/run_sequence.hpp"
#include "runtime/fast_shadow_collect.hpp"

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
// Test-only scripted pad input (runtime/input_script.cpp); absent in benchmark builds.
extern "C" int twv_vita_input_script_load(const char* path) __attribute__((weak));
// patches/dusklight/0015: fixed 30 Hz game ticks, at most N per loop (game clock, plain C entry).
extern "C" void dusk_vita_fixed_ticks(int max_ticks_per_loop) __attribute__((weak));
// --vita-frame-overlap (aurora/frame_overlap.hpp): needs the GX worker; absent in null-graphics builds.
extern "C" int twv_vita_frame_overlap_select(int enabled) __attribute__((weak));

// Explicit CPU allocation budget; renderer allocations stay outside newlib.
// MEM1/ARAM reserves are bounded independently in the Aurora adapter.
extern "C" {
unsigned int _newlib_heap_size_user = 160 * 1024 * 1024;
}

namespace {
// SDK output-buffer ABI (from a data abort on a Vita 1000): arm-vita-eabi uses short enums,
// so out-parameters are structs of the pinned size or word-sized buffers only.
static_assert(sizeof(SceKernelFreeMemorySizeInfo) == 0x10, "sceKernelGetFreeMemorySize output");

// The play profile: g2c's GXM line, applied when the command
// line chooses no renderer option and no GPU backend or feature. Diagnostics (digest, captures,
// GPU timing, shader capture) do not count as a choice. --vita-profile=off keeps every path off.
constexpr const char* PlayProfile[] = {
    "--vita-opt-bundle=on", "--vita-memory-pools=ram64", "--vita-gx-decode=on", "--vita-lazy-vertex-plan=on",
    "--vita-stream-decode=on", "--vita-small-packets=on", "--vita-uncached-staging=on",
    "--vita-static-geometry=on", "--vita-gx-thread=on", "--vita-shadow-image-skip=on", "--vita-pipeline-fast=on",
    "--vita-gpu=gxm", "--vita-gxm-resident=on", "--vita-gxm-compact=colors", "--vita-gxm-primitives=on"};
// Real-shadow ground polygons (patches/dusklight/0018) with the play profile when the command line
// does not choose: on, after Vita3K verify runs (title veil shot, Ordon, the opening) found every
// fast list identical to the original. It takes the veil shot's tick from 7.4 to 2.4 ms in Vita3K.
constexpr int FastShadowCollectPlay = twv::vita::FastShadowCollectOn;
// Frame overlap (aurora/frame_overlap.hpp) with the play profile when the command line does not
// choose: on. Console g6, Ordon at 75 %: 20.5 FPS off, 25.9 FPS on, both at 30 game ticks/s; the
// main thread's wait for the GX worker went from 16.2 to 6.7 ms per frame. A Vita3K verify run found
// no vertex array or static display list changed by the overlapped tick (632,213 and 789,040 checks).
constexpr int FrameOverlapPlay = 1;

// "<prefix><a>" -> choice 0, "<prefix><b>" -> choice 1 (returns 1); another value or a second
// time -> -1; any other token -> 0.
int wordOption(const char* token, const char* prefix, const char* a, const char* b, int& choice) {
    const std::size_t n = std::strlen(prefix);
    if (std::strncmp(token, prefix, n) != 0) return 0;
    if (choice >= 0) return -1;
    if (std::strcmp(token + n, a) == 0) choice = 0;
    else if (std::strcmp(token + n, b) == 0) choice = 1;
    else return -1;
    return 1;
}

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
    bool haveArgs = false, fromSequence = false;
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
            haveArgs = fromSequence = true;
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
                twv::vita::telemetryFlush();
                sceKernelExitProcess(2);
                return 2;
            }
            extra[n] = 0;
            haveArgs = true;
        }
    }
    // Port-owned option: 1 consumed, 0 not ours (a game argument), -1 rejected (the receipt is
    // written; the caller exits). Typed options and the play profile go through the same path.
    const auto portOption = [&](const char* token) -> int {
        const int clockOption = clockOptions.consume(token);
        if (clockOption < 0) {
            twv::vita::telemetryText("args_error", "clock mode invalid or selected more than once; use --vita-clocks=system|legacy444");
            return -1;
        }
        if (clockOption > 0) return 1;
        const int exitOption = exitAfter.consume(token);
        if (exitOption < 0) {
            twv::vita::telemetryText("args_error", "--vita-exit-after needs 10..3600 seconds, once");
            return -1;
        }
        if (exitOption > 0) return 1;
        const int gpuOption = gpuOptions.consume(token);
        if (gpuOption < 0) {
            twv::vita::telemetryText("args_error", "GPU option invalid or selected more than once; see runtime/gpu_options.hpp");
            return -1;
        }
        if (gpuOption > 0) {
            twv::vita::telemetryText("gpu_option", token);
            return 1;
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
            return -1;
        }
        if(rendererOption==1) {
            twv::vita::telemetryText("vertex_stream_requested",rendererOptions.streamDecode?"on":"off");
            return 1;
        }
        if(rendererOption==2) {
            twv::vita::telemetryText("geometry_neon_requested",rendererOptions.neonCompare?"on":"off");
            return 1;
        }
        if(rendererOption==3) {
            twv::vita::telemetryText("shadow_image_skip_requested",rendererOptions.shadowImageSkip?"on":"off");
            return 1;
        }
        if(rendererOption==4) {
            twv::vita::telemetryText("vertex_cull_requested",rendererOptions.vertexCull?"on":"off");
            return 1;
        }
        if(rendererOption==5) {
            twv::vita::telemetryText("sparse_spans_requested",rendererOptions.sparseSpans?"on":"off");
            return 1;
        }
        if(rendererOption==12 || rendererOption==13) {
            twv::vita::telemetryText(rendererOption==12?"small_packets_requested":"uncached_staging_requested",
                (rendererOption==12?rendererOptions.smallPackets:rendererOptions.uncachedStaging)?"on":"off");
            return 1;
        }
        if(rendererOption==14) {
            twv::vita::telemetryText("gx_decode_requested",rendererOptions.gxDecode?"on":"off");
            return 1;
        }
        if(rendererOption==15) {
            twv::vita::telemetryText("lazy_vertex_plan_requested",rendererOptions.lazyVertexPlan?"on":"off");
            return 1;
        }
        if(rendererOption==19) {
            const char* names[]={"off","on","verify"};
            twv::vita::telemetryText("neon_decode_requested",names[rendererOptions.neonDecode]);
            return 1;
        }
        if(rendererOption==20) {
            const char* names[]={"off","on","verify"};
            twv::vita::telemetryText("pipeline_fast_requested",names[rendererOptions.pipelineFast]);
            return 1;
        }
        if(rendererOption==18) {
            twv::vita::telemetryText("shader_capture_requested",rendererOptions.shaderCapture?"on":"off");
            return 1;
        }
        if(rendererOption==17) {
            twv::vita::telemetryText("gx_thread_requested",rendererOptions.gxThread?"on":"off");
            return 1;
        }
        if(rendererOption==16) {
            const char* names[]={"off","on","verify"};
            twv::vita::telemetryText("static_geometry_requested",names[rendererOptions.staticGeometry]);
            return 1;
        }
        if(rendererOption>=6) {
            twv::vita::telemetryText("render_bundle_option",token);
            return 1;
        }
        return 0;
    };
    // Read here only:
    // --vita-profile=play|off: the play profile. Implicit (play) for no args.txt, or one that chooses
    //   no renderer option, GPU backend or feature and no --vita-ticks (clocks, exit timers, receipts,
    //   input scripts and diagnostics do not count); never for a sequence.txt run. Explicitly with
    //   diagnostics only.
    // --vita-ticks=fixed|frames|single: game ticks at a fixed 30 Hz, at most 2 per loop (patches/
    //   dusklight/0015), or one per presented frame as before. Default fixed with the play profile.
    //   single: fixed, at most one per loop, so a run slower than 30 FPS (the emulator) runs exactly
    //   one tick per frame and frames match between runs (A/B captures).
    // --vita-receipts=full|quiet: default quiet with the implicit play profile (free play).
    // --vita-frame-overlap=on|off|verify: the next game tick runs while the GX worker finishes the last
    //   frame (aurora/frame_overlap.hpp). Default on with the play profile (FrameOverlapPlay), off
    //   otherwise. Needs the GX worker (the play profile has it);
    //   not a renderer choice, so the implicit play profile stays. verify: frames end as with off, and
    //   game memory the frame read in place is checked for changes made by the next tick.
    // --vita-fast-shadow-collect=off|on|verify (runtime/fast_shadow_collect.hpp): not a renderer
    //   choice, so it leaves the play profile alone. Default: FastShadowCollectPlay with the profile.
    int profileChoice = -1, receiptsChoice = -1, ticksChoice = -1, overlapChoice = -1, fastShadowChoice = -1;
    bool inputScript = false;
    if (haveArgs) {
        for (char* token = std::strtok(extra, " \t\r\n"); token; token = std::strtok(nullptr, " \t\r\n")) {
            if (std::strcmp(token, "--vita-frame-overlap=verify") == 0) {
                if (overlapChoice >= 0) {
                    twv::vita::telemetryText("args_error", "--vita-frame-overlap needs on, off or verify, once");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                overlapChoice = 2;
                continue;
            }
            if (const int choice = wordOption(token, "--vita-frame-overlap=", "off", "on", overlapChoice)) {
                if (choice < 0) {
                    twv::vita::telemetryText("args_error", "--vita-frame-overlap needs on, off or verify, once");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                continue;
            }
            if (const int choice = twv::vita::fastShadowCollectOption(token, fastShadowChoice)) {
                if (choice < 0) {
                    twv::vita::telemetryText("args_error", "--vita-fast-shadow-collect needs off, on or verify, once");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                continue;
            }
            if (std::strcmp(token, "--vita-ticks=single") == 0) {
                if (ticksChoice >= 0) {
                    twv::vita::telemetryText("args_error", "--vita-ticks needs fixed, frames or single, once");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                ticksChoice = 2;
                continue;
            }
            if (const int choice = wordOption(token, "--vita-ticks=", "frames", "fixed", ticksChoice)) {
                if (choice < 0) {
                    twv::vita::telemetryText("args_error", "--vita-ticks needs fixed, frames or single, once");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                continue;
            }
            if (const int choice = wordOption(token, "--vita-profile=", "off", "play", profileChoice)) {
                if (choice < 0) {
                    twv::vita::telemetryText("args_error", "--vita-profile needs play or off, once");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                continue;
            }
            if (const int choice = wordOption(token, "--vita-receipts=", "full", "quiet", receiptsChoice)) {
                if (choice < 0) {
                    twv::vita::telemetryText("args_error", "--vita-receipts needs full or quiet, once");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                continue;
            }
            if (std::strncmp(token, "--vita-input-script=", 20) == 0) {
                // A file in the data folder: letters, digits, '.', '_' and '-' only.
                const char* name = token + 20;
                bool valid = *name && std::strlen(name) <= 64 && !inputScript;
                for (const char* c = name; valid && *c; ++c)
                    valid = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                            *c == '.' || *c == '_' || *c == '-';
                char path[160];
                std::snprintf(path, sizeof path, "%s/%s", twv::vita::DataRoot, name);
                if (!valid || !twv_vita_input_script_load || !twv_vita_input_script_load(path)) {
                    twv::vita::telemetryText("args_error", "--vita-input-script needs one readable, valid script in the data folder");
                    twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
                }
                inputScript = true;
                twv::vita::telemetryText("input_script", name);
                continue;
            }
            const int port = portOption(token);
            if (port < 0) {twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;}
            if (port > 0) continue;
            if(std::strncmp(token,"--vita-benchmark=",17)==0) {
#if TWV_VITA_BENCHMARK
                if(twv_vita_benchmark_enable(token+17))continue;
#endif
                twv::vita::telemetryText("args_error","benchmark unavailable, unknown or selected more than once");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
            }
            if (gameArgc == 31) {
                twv::vita::telemetryText("args_error", "too many arguments");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);
                return 2;
            }
            gameArgs[gameArgc++] = token;
            twv::vita::telemetryText("extra_arg", token);
        }
    }
    // Play profile, through the same parse-and-select path as typed options: it applies only when
    // no renderer or backend option was typed, so each selector still runs once. Only in builds
    // with every path it needs (the perf build); elsewhere the old all-off defaults stay.
    const bool explicitRenderer = rendererOptions.any_choice() || gpuOptions.any_choice();
    const bool profileBuilt = twv_vita_stream_decode_select && twv_vita_shadow_image_skip_select &&
        twv_vita_small_packets_select && twv_vita_uncached_staging_select && twv_vita_gx_fast_select &&
        twv_vita_lazy_vertex_plan_select && twv_vita_static_geometry_select && twv_vita_gx_thread_select &&
        twv_vita_pipeline_fast_select && twv_vita_gpu_select;
    if (profileChoice == 1 && (explicitRenderer || !profileBuilt)) {
        twv::vita::telemetryText("args_error", "--vita-profile=play with explicit renderer options or in a build without its paths");
        twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
    }
    const bool implicitProfile = profileChoice < 0 && !fromSequence && !explicitRenderer && ticksChoice < 0 &&
        profileBuilt;
    const bool playProfile = profileChoice == 1 || implicitProfile;
    if (playProfile)
        for (const char* token : PlayProfile)
            if (portOption(token) != 1) {
                twv::vita::telemetryText("args_error", "play profile option rejected");
                twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
            }
    // Render size 75 % (720x408, scaled up by the display) unless the command line chose one. Drawn
    // full width, Link's house needs about 67 ms of GPU per frame at 100 % and about 43 at 75 %
    // (fitted from g4 RUN1/RUN2 on the console). --vita-gxm-scale=100 keeps the full size.
    if (playProfile && !gpuOptions.scaleSelected && portOption("--vita-gxm-scale=75") != 1) {
        twv::vita::telemetryText("args_error", "play profile render scale rejected");
        twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
    }
    twv::vita::telemetryText("play_profile", implicitProfile ? "on" : playProfile ? "on (explicit)" :
                             profileChoice == 0 ? "off" : fromSequence ? "off (sequence run)" :
                             explicitRenderer ? "off (renderer or GPU options given)" :
                             ticksChoice >= 0 ? "off (--vita-ticks given)" : "not in this build");
    // Fixed 30 Hz game ticks with the play profile: real game speed below 30 FPS. The frame cap is
    // a --cvar before the user's own arguments, so config.json is never rewritten and a later
    // --cvar still wins.
    const bool fixedTicks = ticksChoice >= 0 ? ticksChoice >= 1 : playProfile;
    const int maxTicks = ticksChoice == 2 ? 1 : 2;
    if (fixedTicks) {
        static char cvar[] = "--cvar", frameCap[] = "video.maxFrameRate=30";
        if (gameArgc + 2 > 31 || !dusk_vita_fixed_ticks) {
            twv::vita::telemetryText("args_error", "fixed ticks unavailable in this build, or too many arguments");
            twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
        }
        for (int i = gameArgc - 1; i >= 5; --i) gameArgs[i + 2] = gameArgs[i];
        gameArgs[5] = cvar;gameArgs[6] = frameCap;gameArgc += 2;
        dusk_vita_fixed_ticks(maxTicks);
    }
    twv::vita::telemetryEvent("fixed_game_ticks", fixedTicks ? maxTicks : 0);
    // Frame overlap: after the GX thread choice (typed or from the profile), before the game starts.
    // With the play profile and no typed choice: FrameOverlapPlay.
    const int overlap = overlapChoice >= 0 ? overlapChoice : playProfile ? FrameOverlapPlay : -1;
    if (overlap >= 0 && (!twv_vita_frame_overlap_select || !twv_vita_frame_overlap_select(overlap))) {
        twv::vita::telemetryText("args_error", "--vita-frame-overlap=on needs the GX worker (--vita-gx-thread=on, as in the play profile) in a vitaGL/GXM build");
        twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
    }
    twv::vita::telemetryText("frame_overlap_requested", overlapChoice < 0 ?
                             (overlap == 1 ? "default on (play profile)" : "default off") :
                             overlapChoice == 2 ? "verify" : overlapChoice ? "on" : "off");
    const int fastShadow = fastShadowChoice >= 0 ? fastShadowChoice : playProfile ? FastShadowCollectPlay :
        int(twv::vita::FastShadowCollectOff);
    if (!twv_vita_fast_shadow_collect_select(fastShadow)) {
        twv::vita::telemetryText("args_error", "fast shadow collection mode rejected");
        twv::vita::telemetryFlush();sceKernelExitProcess(2);return 2;
    }
    twv::vita::telemetryText(fastShadowChoice >= 0 ? "fast_shadow_collect_requested" : "fast_shadow_collect_default",
                             twv::vita::FastShadowCollectNames[fastShadow]);
    // Quiet receipts with the implicit profile (free play); sequence runs, explicit profiles and
    // renderer choices keep every receipt, and --vita-receipts decides either way.
    twv::vita::telemetryQuiet(receiptsChoice >= 0 ? receiptsChoice == 1 : implicitProfile);
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
