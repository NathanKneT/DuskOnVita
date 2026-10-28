#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// SDK calls are recorded here; no SDK headers, libraries or device are used.
struct SceKernelFreeMemorySizeInfo {
    std::uint32_t size, size_user, size_cdram, size_phycont;
};
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo*);
int sceKernelExitProcess(int);
int scePowerGetArmClockFrequency();
int scePowerGetBusClockFrequency();
int scePowerGetGpuClockFrequency();
int scePowerGetGpuXbarClockFrequency();
int scePowerSetArmClockFrequency(int);
int scePowerSetBusClockFrequency(int);
int scePowerSetGpuClockFrequency(int);
int scePowerSetGpuXbarClockFrequency(int);
// The data folder listing for the missing-disc screen (psp2/io/dirent.h).
using SceUID = int;
struct SceIoDirent {
    char d_name[256];
};
SceUID sceIoDopen(const char*);
int sceIoDread(SceUID, SceIoDirent*);
int sceIoDclose(SceUID);

#define main clockEntryUnderTest
#include "clock_entry_fixture.inc"
#undef main

namespace {
struct EntryState {
    std::vector<std::string> calls, nativeCalls, gameArgs;
    std::map<std::string, long long> events;
    std::map<std::string, std::string> text;
    std::map<std::string, int> selections;
    std::vector<int> exits;
    std::array<unsigned, 4> reads{};
    bool bundleAvailable = true, rendererAvailable = true;
    unsigned bundleCalls = 0, gameCalls = 0, benchmarkCalls = 0;
    std::uint32_t bundleFeatures = 0, memoryProfile = 0;
    unsigned exitAfter = 0;
    int gameResult = 7;
    bool quiet = false, gpuCalled = false;
    int gpuBackend = -1;
    std::uint32_t gpuFeatures = 0;
    int overlap = -1;
    int fastShadow = -1; // twv_vita_fast_shadow_collect_select's argument, -1 when not called
    int saveImport = -1; // twv_vita_save_import_select's argument, -1 when not called
} state;
const char* axes[]{"arm", "bus", "gpu", "gpu_xbar"};
constexpr std::array<int, 4> initial{321, -17, 111, INT_MIN};
constexpr std::array<int, 4> observed{-71, 202, -73, 144};
constexpr std::array<int, 4> setterResults{-101, INT_MIN, 2, -104};

int getClock(unsigned axis) {
    const std::string name = std::string("get_") + axes[axis];
    state.calls.push_back(name); state.nativeCalls.push_back(name);
    assert(state.reads[axis] < 2);
    return state.reads[axis]++ == 0 ? initial[axis] : observed[axis];
}
int setClock(unsigned axis, int value) {
    const std::string name = std::string("set_") + axes[axis] + "=" + std::to_string(value);
    state.calls.push_back(name); state.nativeCalls.push_back(name);
    return setterResults[axis];
}
int selectRenderer(const char* name, int enabled) {
    state.calls.emplace_back(name); state.selections[name] = enabled;
    return state.rendererAvailable;
}
void writeFile(const char* path, const std::string& value) {
    std::ofstream file(path, std::ios::binary);
    assert(file); file.write(value.data(), value.size()); assert(file);
}
// A minimal plain GameCube image for the start-up disc check: header, DOL, FST with one file.
std::string gameCubeImage(const char* id) {
    std::string image(0x3000, '\0');
    const auto be32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) image[at + i] = char(v >> (24 - 8 * i));
    };
    image.replace(0, 6, id);
    be32(0x1C, 0xC2339F3Du);
    be32(0x420, 0x2500); be32(0x424, 0x2600); be32(0x428, 24);
    image[0x2600] = 1; be32(0x2608, 2);
    be32(0x2610, 0x2800); be32(0x2614, 0x800);
    return image;
}
std::string discImage = gameCubeImage("GZ2P01");
// startupErrorExit never returns on the Vita; here it unwinds back to run().
struct StartupExit {
    int code;
};
int run(const char* args, bool disc = true, bool bundle = true, bool renderer = true, int gameResult = 7) {
    state = {}; state.bundleAvailable = bundle; state.rendererAvailable = renderer;
    state.gameResult = gameResult;
    if (args) writeFile("ux0:data/twilight-vita/args.txt", args);
    else std::filesystem::remove("ux0:data/twilight-vita/args.txt");
    if (disc) writeFile("ux0:data/twilight-vita/game.iso", discImage);
    else std::filesystem::remove("ux0:data/twilight-vita/game.iso");
    // Existing entry behavior ignores the process argv; args.txt remains the
    // supported option source, even if a process argument resembles our option.
    char arg0[] = "host", ignored[] = "--vita-clocks=legacy444";
    char* argv[]{arg0, ignored, nullptr};
    try {
        return clockEntryUnderTest(2, argv);
    } catch (const StartupExit& exit) {
        return exit.code;
    }
}
void checkRejected(int result) {
    assert(result == 2 && state.gameCalls == 0 && state.exits == std::vector<int>{2});
    assert(state.nativeCalls.empty());
    assert(state.text.count("args_error"));
    // The screen says what the receipt says.
    assert(state.text.at("startup_screen") == "args_error" && state.text.at("screen_problem") == state.text.at("args_error") + ".");
}
void checkClocks(bool legacy, bool explicitMode) {
    assert(state.gameCalls == 1 && state.bundleCalls == 1);
    assert(state.text.at("clock_mode") == (legacy ? "legacy444" : "system"));
    assert(state.events.at("clock_mode_explicit") == explicitMode);
    assert(state.events.at("clock_setter_attempts") == (legacy ? 4 : 0));
    std::vector<std::string> expected{"get_arm", "get_bus", "get_gpu", "get_gpu_xbar"};
    if (legacy) expected.insert(expected.end(), {"set_arm=444", "set_bus=222", "set_gpu=222", "set_gpu_xbar=166"});
    expected.insert(expected.end(), {"get_arm", "get_bus", "get_gpu", "get_gpu_xbar"});
    assert(state.nativeCalls == expected);
    const auto bundle = std::find(state.calls.begin(), state.calls.end(), "bundle");
    const auto firstClock = std::find(state.calls.begin(), state.calls.end(), "get_arm");
    const auto game = std::find(state.calls.begin(), state.calls.end(), "game");
    assert(bundle < firstClock && firstClock < game);
    const std::array<int, 4> requested{444, 222, 222, 166};
    for (unsigned i = 0; i < 4; ++i) {
        const std::string axis = axes[i];
        assert(state.events.at("clock_initial_" + axis + "_mhz_result") == initial[i]);
        assert(state.events.at("clock_observed_" + axis + "_mhz_result") == observed[i]);
        if (legacy) {
            assert(state.events.at("clock_set_" + axis + "_requested_mhz") == requested[i]);
            assert(state.events.at("clock_set_" + axis + "_result") == setterResults[i]);
        } else {
            assert(!state.events.count("clock_set_" + axis + "_requested_mhz"));
            assert(!state.events.count("clock_set_" + axis + "_result"));
        }
    }
    assert(state.events.at("arm_mhz") == observed[0] && state.events.at("gpu_mhz") == observed[2]);
    assert(state.exits == std::vector<int>{state.gameResult});
    assert(state.gameArgs.size() >= 5);
    assert(std::vector<std::string>(state.gameArgs.begin(), state.gameArgs.begin() + 5) ==
        (std::vector<std::string>{"twilight-vita", "--user-dir", "ux0:data/twilight-vita", "--dvd", "ux0:data/twilight-vita/game.iso"}));
    for (const auto& arg : state.gameArgs) assert(arg.find("--vita-clocks=") != 0);
}
}

int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* info) {
    assert(info->size == sizeof *info); info->size_user = 1024; return 0;
}
int sceKernelExitProcess(int result) { state.exits.push_back(result); return 0; }
int scePowerGetArmClockFrequency() { return getClock(0); }
int scePowerGetBusClockFrequency() { return getClock(1); }
int scePowerGetGpuClockFrequency() { return getClock(2); }
int scePowerGetGpuXbarClockFrequency() { return getClock(3); }
int scePowerSetArmClockFrequency(int value) { return setClock(0, value); }
int scePowerSetBusClockFrequency(int value) { return setClock(1, value); }
int scePowerSetGpuClockFrequency(int value) { return setClock(2, value); }
int scePowerSetGpuXbarClockFrequency(int value) { return setClock(3, value); }
namespace twv::vita {
void telemetryInit() { state.calls.emplace_back("init"); }
void installFaultReceipts() { state.calls.emplace_back("fault_receipts"); }
void telemetryEvent(const char* name, long long value) { state.events[name] = value; }
void telemetryText(const char* name, const char* value) { state.text[name] = value; }
void telemetryFlush() {}
void telemetrySync() { state.calls.emplace_back("sync"); }
void runControlExitAfter(unsigned seconds) { state.calls.emplace_back("exit_after"); state.exitAfter = seconds; }
void telemetryQuiet(bool quiet) { state.quiet = quiet; }
void startupErrorExit(const MessageText& text, int code) {
    state.text["startup_screen"] = text.kind;
    state.text["screen_title"] = text.message.title;
    state.text["screen_problem"] = text.message.items[0].text ? text.message.items[0].text : "";
    for (const auto& item : text.message.items)
        if (item.text && item.style == ui::Style::Path) state.text["screen_path"] = item.text;
    state.exits.push_back(code);
    throw StartupExit{code};
}
}
std::vector<std::string> folder; // what sceIoDread lists
std::size_t folderNext = 0;
SceUID sceIoDopen(const char* path) {
    folderNext = 0;
    return std::string(path) == "ux0:data/twilight-vita" ? 5 : -1;
}
int sceIoDread(SceUID dir, SceIoDirent* entry) {
    assert(dir == 5);
    if (folderNext >= folder.size()) return 0;
    std::snprintf(entry->d_name, sizeof entry->d_name, "%s", folder[folderNext++].c_str());
    return 1;
}
int sceIoDclose(SceUID dir) { assert(dir == 5); return 0; }
#if TWV_TEST_PLAY_PROFILE
// With a GPU selector present, a build has every path the play profile needs.
extern "C" int twv_vita_gpu_select(int backend, int, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                   std::uint32_t, int, std::uint32_t features, std::uint32_t) {
    state.calls.emplace_back("gpu"); state.gpuCalled = true; state.gpuBackend = backend; state.gpuFeatures = features;
    return 1;
}
int fixedTicks = -1;
extern "C" void dusk_vita_fixed_ticks(int ticks) { fixedTicks = ticks; }
#endif
extern "C" int twv_vita_stream_decode_select(int value) { return selectRenderer("stream", value); }
extern "C" int twv_vita_neon_compare_select(int value) { return selectRenderer("neon", value); }
extern "C" int twv_vita_shadow_image_skip_select(int value) { return selectRenderer("shadow", value); }
extern "C" int twv_vita_vertex_cull_select(int value) { return selectRenderer("cull", value); }
extern "C" int twv_vita_sparse_spans_select(int value) { return selectRenderer("sparse", value); }
extern "C" int twv_vita_small_packets_select(int value) { return selectRenderer("small", value); }
extern "C" int twv_vita_uncached_staging_select(int value) { return selectRenderer("uncached", value); }
extern "C" int twv_vita_gx_fast_select(int value) noexcept { return selectRenderer("gx", value); }
extern "C" int twv_vita_lazy_vertex_plan_select(int value) { return selectRenderer("lazy_plan", value); }
extern "C" int twv_vita_static_geometry_select(int value) { return selectRenderer("static", value); }
extern "C" int twv_vita_gx_thread_select(int value) { return selectRenderer("gx_thread", value); }
extern "C" int twv_vita_gx_thread_pin_main() { state.calls.emplace_back("pin_main"); return 0; }
// As aurora_vita.cpp: on needs the GX worker chosen first.
extern "C" int twv_vita_frame_overlap_select(int value) {
    state.calls.emplace_back("frame_overlap"); state.overlap = value;
    return !value || (state.selections.count("gx_thread") && state.selections.at("gx_thread"));
}
extern "C" int twv_vita_shader_capture_select(int value) { return selectRenderer("capture", value); }
extern "C" int twv_vita_neon_decode_select(int value) { return selectRenderer("neon_decode", value); }
extern "C" int twv_vita_pipeline_fast_select(int value) { return selectRenderer("pipeline_fast", value); }
// Not a renderer choice: recorded apart, so the play profile's selections stay as they are.
extern "C" int twv_vita_fast_shadow_collect_select(int mode) {
    assert(state.fastShadow == -1); state.fastShadow = mode; return mode >= 0 && mode <= 2;
}
// --vita-import-saves (aurora/save_safety.cpp): not a renderer choice either; set once per boot.
extern "C" int twv_vita_save_import_select(int mode) {
    assert(state.saveImport == -1); state.saveImport = mode; return 1;
}
extern "C" int twv_vita_bundle_select(std::uint32_t features, std::uint32_t memory) {
    state.calls.emplace_back("bundle"); ++state.bundleCalls;
    state.bundleFeatures = features; state.memoryProfile = memory; return state.bundleAvailable;
}
extern "C" std::uint32_t twv_vita_bundle_compiled() { return 1; }
#if TWV_VITA_BENCHMARK
extern "C" int twv_vita_benchmark_enable(const char* name) {
    state.calls.emplace_back("benchmark");
    return ++state.benchmarkCalls == 1 && std::string(name) == "C1-FIGHT";
}
#endif
int game_main(int argc, char* argv[]) {
    state.calls.emplace_back("game"); ++state.gameCalls;
    assert(argv[argc] == nullptr);
    for (int i = 0; i < argc; ++i) state.gameArgs.emplace_back(argv[i]);
    return state.gameResult;
}

int main() {
    const auto previous = std::filesystem::current_path();
    auto pattern = (std::filesystem::temp_directory_path() / "twv-clock-entry-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data()); assert(directory);
    const auto root = std::filesystem::path(directory);
    std::filesystem::current_path(root);
    std::filesystem::create_directories("ux0:data/twilight-vita");
#if TWV_TEST_PLAY_PROFILE
    // Play profile, implicit: no args.txt (or one without any port option) -> g2c's GXM line through
    // the same selectors, fixed game ticks with a 30 FPS cap before the user's arguments, quiet receipts.
    fixedTicks = -1;
    assert(run(nullptr) == 7); checkClocks(true, false);
    // Features: primitives, compact colours, resident (15) and the 75 % render size (64).
    assert(state.text.at("play_profile") == "on" && state.gpuBackend == 1 && state.gpuFeatures == 79u);
    assert(state.selections == (std::map<std::string, int>{{"stream", 1}, {"shadow", 1}, {"small", 1},
        {"uncached", 1}, {"gx", 1}, {"lazy_plan", 1}, {"static", 1}, {"gx_thread", 1}, {"pipeline_fast", 1}}));
    assert(state.bundleFeatures == TWV_BUNDLE_CORE && state.memoryProfile == 2 && state.quiet);
    assert(state.events.at("gx_thread_main_affinity_result") == 0);
    assert(fixedTicks == 2 && state.events.at("fixed_game_ticks") == 2);
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--cvar", "video.maxFrameRate=30"}));
    assert(state.fastShadow == FastShadowCollectPlay &&
           state.text.at("fast_shadow_collect_default") == twv::vita::FastShadowCollectNames[FastShadowCollectPlay]);
    // --vita-fast-shadow-collect is not a renderer choice: the implicit profile stays on.
    fixedTicks = -1;
    assert(run("--vita-fast-shadow-collect=verify --stage R_SP01,4,2,0") == 7 && state.text.at("play_profile") == "on");
    assert(state.fastShadow == 2 && state.text.at("fast_shadow_collect_requested") == "verify" && fixedTicks == 2);
    assert(state.selections.at("gx_thread") == 1 && state.gameArgs.back() == "R_SP01,4,2,0");
    // --vita-import-saves leaves the implicit profile on too.
    assert(run("--vita-import-saves=off") == 7 && state.text.at("play_profile") == "on" && state.saveImport == 0);
    assert(run("--vita-profile=off --vita-fast-shadow-collect=on") == 7 && state.fastShadow == 1);
    assert(state.text.at("play_profile") == "off" && state.selections.empty());
    fixedTicks = -1;
    assert(run("--stage R_SP01,4,2,0 --cvar video.maxFrameRate=20") == 7 && state.text.at("play_profile") == "on");
    assert(fixedTicks == 2 && std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--cvar", "video.maxFrameRate=30", "--stage", "R_SP01,4,2,0", "--cvar",
                                  "video.maxFrameRate=20"}));
    // Clocks, exit timers, receipts and diagnostics left in args.txt keep the implicit profile.
    fixedTicks = -1;
    assert(run("--vita-clocks=legacy444") == 7); checkClocks(true, true);
    assert(state.text.at("play_profile") == "on" && state.gpuBackend == 1 && state.gpuFeatures == 79u);
    assert(state.selections.at("gx_thread") == 1 && state.quiet && fixedTicks == 2);
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--cvar", "video.maxFrameRate=30"}));
    // A typed render size keeps the profile and wins over its 75 %: 100 % adds no feature, 67 % adds 128.
    fixedTicks = -1;
    assert(run("--vita-gxm-scale=100") == 7 && state.text.at("play_profile") == "on" && state.gpuFeatures == 15u);
    fixedTicks = -1;
    assert(run("--vita-gxm-scale=67") == 7 && state.text.at("play_profile") == "on" && state.gpuFeatures == 143u);
    fixedTicks = -1;
    assert(run("--vita-exit-after=60 --vita-shader-capture=on --vita-receipts=full --stage R_SP01,4,2,0") == 7);
    assert(state.text.at("play_profile") == "on" && state.selections.at("capture") == 1);
    assert(state.selections.at("static") == 1 && state.gpuBackend == 1 && !state.quiet && fixedTicks == 2);
    // A renderer, GPU backend or feature choice, or --vita-ticks, turns it off (the old all-off defaults
    // for everything not chosen, full receipts).
    fixedTicks = -1;
    assert(run("--vita-gx-thread=on --stage R_SP01,4,2,0") == 7);
    assert(state.text.at("play_profile") == "off (renderer or GPU options given)" && state.selections.size() == 1);
    assert(!state.gpuCalled && !state.quiet && fixedTicks == -1 && state.gameArgs.size() == 7);
    assert(run("--vita-gpu=vgl") == 7 && state.text.at("play_profile") == "off (renderer or GPU options given)");
    assert(state.selections.empty() && state.gpuBackend == 0 && !state.quiet && fixedTicks == -1);
    assert(run("--vita-ticks=frames") == 7 && state.text.at("play_profile") == "off (--vita-ticks given)");
    assert(state.selections.empty() && !state.gpuCalled && !state.quiet && fixedTicks == -1);
    // Explicit: diagnostics allowed, renderer or backend choices refused; ticks and receipts overridable.
    assert(run("--vita-profile=play --vita-shader-capture=on --vita-exit-after=60 --stage R_SP01,4,2,0") == 7);
    assert(state.text.at("play_profile") == "on (explicit)" && state.selections.at("capture") == 1);
    assert(state.selections.at("static") == 1 && state.gpuBackend == 1 && !state.quiet && fixedTicks == 2);
    fixedTicks = -1;
    assert(run("--vita-profile=play --vita-ticks=frames") == 7 && fixedTicks == -1 && state.gameArgs.size() == 5);
    // single: fixed ticks, at most one per loop (emulator A/B runs), with the same frame cap.
    fixedTicks = -1;
    assert(run("--vita-profile=play --vita-ticks=single") == 7 && fixedTicks == 1);
    assert(state.events.at("fixed_game_ticks") == 1 && state.text.at("play_profile") == "on (explicit)");
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--cvar", "video.maxFrameRate=30"}));
    assert(run("--vita-ticks=single") == 7 && state.text.at("play_profile") == "off (--vita-ticks given)");
    assert(run("--vita-profile=play --vita-receipts=quiet") == 7 && state.quiet);
    assert(run("--vita-profile=off --stage R_SP01,4,2,0") == 7 && state.text.at("play_profile") == "off");
    assert(state.selections.empty() && !state.gpuCalled && state.gameArgs.size() == 7);
    for (const char* bad : {"--vita-profile=play --vita-gx-thread=on", "--vita-profile=play --vita-gpu=vgl",
                            "--vita-profile=maybe", "--vita-profile=off --vita-profile=off", "--vita-receipts=loud",
                            "--vita-ticks=sometimes", "--vita-ticks=fixed --vita-ticks=fixed",
                            "--vita-ticks=single --vita-ticks=single", "--vita-ticks=fixed --vita-ticks=single",
                            "--vita-fast-shadow-collect=maybe", "--vita-fast-shadow-collect=",
                            "--vita-fast-shadow-collect=on --vita-fast-shadow-collect=on"})
        checkRejected(run(bad));
    // --vita-frame-overlap: on by default with the play profile (selected like a typed =on), off
    // without it (no selector call); on/off keep the implicit profile and are selected after its GX
    // worker, before the game; on needs the worker; a bad or repeated value fails.
    assert(run(nullptr) == 7 && state.overlap == 1 &&
           state.text.at("frame_overlap_requested") == "default on (play profile)");
    {
        const auto thread = std::find(state.calls.begin(), state.calls.end(), "gx_thread");
        const auto overlap = std::find(state.calls.begin(), state.calls.end(), "frame_overlap");
        const auto game = std::find(state.calls.begin(), state.calls.end(), "game");
        assert(thread < overlap && overlap < game);
    }
    assert(run("--vita-profile=off") == 7 && state.overlap == -1 && state.text.at("frame_overlap_requested") == "default off");
    assert(run("--vita-gx-thread=on") == 7 && state.overlap == -1 && state.text.at("frame_overlap_requested") == "default off");
    fixedTicks = -1;
    assert(run("--vita-frame-overlap=on --stage R_SP01,4,2,0") == 7 && state.text.at("play_profile") == "on");
    assert(state.overlap == 1 && state.text.at("frame_overlap_requested") == "on" && fixedTicks == 2);
    assert(state.selections.at("gx_thread") == 1 && state.gpuFeatures == 79u && state.quiet);
    {
        const auto thread = std::find(state.calls.begin(), state.calls.end(), "gx_thread");
        const auto overlap = std::find(state.calls.begin(), state.calls.end(), "frame_overlap");
        const auto game = std::find(state.calls.begin(), state.calls.end(), "game");
        assert(thread < overlap && overlap < game);
    }
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--cvar", "video.maxFrameRate=30", "--stage", "R_SP01,4,2,0"}));
    assert(run("--vita-frame-overlap=off") == 7 && state.overlap == 0 && state.text.at("play_profile") == "on");
    assert(state.text.at("frame_overlap_requested") == "off");
    assert(run("--vita-gx-thread=on --vita-frame-overlap=on") == 7 && state.overlap == 1);
    assert(state.text.at("play_profile") == "off (renderer or GPU options given)");
    assert(run("--vita-profile=off --vita-frame-overlap=off") == 7 && state.overlap == 0);
    // verify: mode 2, same requirements.
    assert(run("--vita-frame-overlap=verify") == 7 && state.overlap == 2 && state.text.at("play_profile") == "on");
    assert(state.text.at("frame_overlap_requested") == "verify");
    for (const char* bad : {"--vita-profile=off --vita-frame-overlap=on", "--vita-gx-thread=off --vita-frame-overlap=on",
                            "--vita-frame-overlap=maybe", "--vita-frame-overlap=on --vita-frame-overlap=on",
                            "--vita-profile=off --vita-frame-overlap=verify", "--vita-frame-overlap=verify --vita-frame-overlap=verify",
                            "--vita-frame-overlap=on --vita-frame-overlap=verify", "--vita-frame-overlap=verify --vita-frame-overlap=on"})
        checkRejected(run(bad));
    // Never implicit for a sequence run: vitaGL reference lines stay vitaGL.
    writeFile("ux0:data/twilight-vita/sequence.txt", "o1: --stage F_SP103,1,3,-1\n");
    std::filesystem::remove("ux0:data/twilight-vita/sequence_state.txt");
    assert(run(nullptr) == 7 && !state.quiet && state.text.at("play_profile") == "off (sequence run)");
    assert(state.selections.empty() && !state.gpuCalled);
    std::filesystem::remove("ux0:data/twilight-vita/sequence.txt");
    std::filesystem::remove("ux0:data/twilight-vita/sequence_state.txt");
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(root);
    std::puts("clock_entry_test: implicit play profile unless renderer/GPU choices or --vita-ticks (clocks, timers, diagnostics kept), explicit with diagnostics only, never for sequence runs; fixed ticks, frame cap before user args, quiet receipts passed");
    return 0;
#endif
    assert(run(nullptr) == 7); checkClocks(true, false); assert(state.gameArgs.size() == 5);
    assert(state.exitAfter == 0 && !state.events.count("sequence_index") && !state.events.count("sequence_complete"));
    // No play profile in this build: the fast shadow collection is off unless chosen.
    assert(state.fastShadow == 0 && state.text.at("fast_shadow_collect_default") == "off");
    assert(run("--vita-fast-shadow-collect=on --vita-clocks=system") == 7); checkClocks(false, true);
    assert(state.fastShadow == 1 && state.text.at("fast_shadow_collect_requested") == "on" && state.gameArgs.size() == 5);
    for (const char* bad : {"--vita-fast-shadow-collect=ON", "--vita-fast-shadow-collect=onx",
                            "--vita-fast-shadow-collect=off --vita-fast-shadow-collect=verify"})
        checkRejected(run(bad));
    // --vita-import-saves: auto unless off, passed on before the game starts, never a game argument.
    assert(run(nullptr) == 7 && state.saveImport == 1 && state.text.at("save_import_mode") == "default auto");
    assert(run("--vita-import-saves=off --vita-clocks=system") == 7); checkClocks(false, true);
    assert(state.saveImport == 0 && state.text.at("save_import_mode") == "off" && state.gameArgs.size() == 5);
    assert(run("--vita-import-saves=auto") == 7 && state.saveImport == 1 && state.text.at("save_import_mode") == "auto");
    for (const char* bad : {"--vita-import-saves=on", "--vita-import-saves=", "--vita-import-saves=OFF",
                            "--vita-import-saves=off --vita-import-saves=auto"})
        checkRejected(run(bad));
    assert(run("--vita-clocks=system") == 7); checkClocks(false, true);
    assert(run("--vita-clocks=legacy444") == 7); checkClocks(true, true);
    assert(run("--stage R_SP01,4,2,0 --vita-clocks=legacy444 --spawn 1") == 7);
    checkClocks(true, true);
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--stage", "R_SP01,4,2,0", "--spawn", "1"}));
    assert(run("--vita-stream-decode=on --vita-clocks=legacy444 --vita-opt-bundle=on "
               "--vita-compact-st=off --vita-memory-pools=ram64 --vita-small-packets=on "
               "--vita-uncached-staging=off --vita-neon-compare=on --vita-shadow-image-skip=off "
               "--vita-vertex-cull=on --vita-sparse-spans=off") == 7);
    checkClocks(true, true);
    assert(state.gameArgs.size() == 5 && state.bundleFeatures == 6 && state.memoryProfile == 2);
    assert(state.selections == (std::map<std::string, int>{{"stream", 1}, {"neon", 1}, {"shadow", 0},
        {"cull", 1}, {"sparse", 0}, {"small", 1}, {"uncached", 0}}));
    assert(run("--vita-clocks=legacy444 --vita-gx-decode=on --stage R_SP01,4,2,0") == 7);
    checkClocks(true, true);
    assert(state.selections.at("gx") == 1 && state.text.at("gx_decode_requested") == "on");
    assert(state.gameArgs.size() == 7);
    for(const char* bad : {"--vita-gx-decode", "--vita-gx-decode=ON",
                          "--vita-clocks=legacy444 --vita-gx-decode=on --vita-gx-decode=off"})
        checkRejected(run(bad));
    checkRejected(run("--vita-clocks=legacy444 --vita-gx-decode=on", true, true, false));
    assert(run("--vita-clocks=legacy444 --vita-lazy-vertex-plan=on --stage R_SP01,4,2,0") == 7);
    checkClocks(true,true);
    assert(state.selections.at("lazy_plan")==1&&state.text.at("lazy_vertex_plan_requested")=="on");
    assert(state.gameArgs.size()==7);
    assert(run("--vita-lazy-vertex-plan=off")==7);checkClocks(true,false);
    assert(state.selections.at("lazy_plan")==0&&state.gameArgs.size()==5);
    for(const char* bad:{"--vita-lazy-vertex-plan","--vita-lazy-vertex-plan=", "--vita-lazy-vertex-plan=ON",
                        "--vita-lazy-vertex-plan=1","--vita-lazy-vertex-plan=onx",
                        "--vita-clocks=legacy444 --vita-lazy-vertex-plan=on --vita-lazy-vertex-plan=off"})
        checkRejected(run(bad));
    checkRejected(run("--vita-clocks=legacy444 --vita-lazy-vertex-plan=on",true,true,false));
    assert(run("--vita-lazy-vertex-plan-extra=on")==7);checkClocks(true,false);
    assert(state.gameArgs.back()=="--vita-lazy-vertex-plan-extra=on");
    for (const char* bad : {"--vita-clocks", "--vita-clocks=", "--vita-clocks=444",
                            "--vita-clocks=System", "--vita-clocks=system=legacy444",
                            "--vita-clocks=legacy444 --vita-clocks=system",
                            "--vita-clocks=system --vita-clocks=system",
                            "--vita-clocks=legacy444 --vita-clocks=legacy444",
                            "--vita-clocks=legacy444 --vita-opt-bundle=invalid",
                            "--vita-opt-bundle=invalid --vita-clocks=legacy444",
                            "--vita-clocks=legacy444 --vita-stream-decode=on --vita-stream-decode=off",
                            "--vita-clocks=legacy444 --vita-benchmark=unknown"}) checkRejected(run(bad));
    checkRejected(run("--vita-clocks=legacy444", true, false));
    checkRejected(run("--vita-clocks=legacy444 --vita-stream-decode=on", true, true, false));
    std::string tooMany = "--vita-clocks=legacy444";
    for (unsigned i = 0; i < 26; ++i) tooMany += " x";
    assert(run(tooMany.c_str()) == 7); checkClocks(true, true);
    assert(state.gameArgs.size() == 31);
    tooMany += " x";
    checkRejected(run(tooMany.c_str()));
    checkRejected(run((std::string("--vita-clocks=legacy444 ") + std::string(512, ' ')).c_str()));
    assert(run(std::string(511, ' ').c_str()) == 7); checkClocks(true, false);
    assert(run("--vita-clocks=legacy444", false) == 2);
    assert(state.nativeCalls.empty() && state.gameCalls == 0 && state.text.count("no_disc"));
    // Game-owned option semantics are validated once by the original game.
    // Their result is preserved; the port consumes only its own clock option.
    assert(run("--game-owned-option --vita-clocks=legacy444", true, true, true, 23) == 23);
    checkClocks(true, true); assert(state.gameArgs.back() == "--game-owned-option");
    assert(run("--vita-clocks-extra=system") == 7); checkClocks(true, false);
    assert(state.gameArgs.back() == "--vita-clocks-extra=system");
    // T3 static geometry: a three-way start-up choice, passed to the renderer, not to the game.
    assert(run("--vita-static-geometry=verify --stage R_SP01,4,2,0") == 7); checkClocks(true, false);
    assert(state.selections.at("static") == 2 && state.text.at("static_geometry_requested") == "verify");
    assert(state.gameArgs.size() == 7);
    assert(run("--stage R_SP01,4,2,0") == 7);
    assert(!state.selections.count("static") && state.text.at("static_geometry_requested") == "default off");
    for (const char* bad : {"--vita-static-geometry=1", "--vita-static-geometry=on --vita-static-geometry=off"})
        checkRejected(run(bad));
    checkRejected(run("--vita-static-geometry=on", true, true, false));
    // T2 GX worker thread: selected for Aurora, and the game thread pinned after the clocks.
    assert(run("--vita-gx-thread=on --stage R_SP01,4,2,0") == 7); checkClocks(true, false);
    assert(state.selections.at("gx_thread") == 1 && state.text.at("gx_thread_requested") == "on");
    assert(state.events.at("gx_thread_main_affinity_result") == 0 && state.gameArgs.size() == 7);
    {
        const auto pin = std::find(state.calls.begin(), state.calls.end(), "pin_main");
        const auto lastClock = std::find(state.calls.rbegin(), state.calls.rend(), "get_gpu_xbar");
        const auto game = std::find(state.calls.begin(), state.calls.end(), "game");
        assert(pin != state.calls.end() && lastClock.base() <= pin && pin < game);
    }
    assert(run("--vita-gx-thread=off") == 7); checkClocks(true, false);
    assert(state.selections.at("gx_thread") == 0 && !state.events.count("gx_thread_main_affinity_result"));
    assert(run("") == 7 && state.text.at("gx_thread_requested") == "default off");
    for (const char* bad : {"--vita-gx-thread=1", "--vita-gx-thread=on --vita-gx-thread=on"}) checkRejected(run(bad));
    checkRejected(run("--vita-gx-thread=on", true, true, false));
    // Shader source capture: a renderer start-up choice, never passed to the game.
    assert(run("--vita-shader-capture=on") == 7 && state.selections.at("capture") == 1);
    assert(state.text.at("shader_capture_requested") == "on" && state.gameArgs.size() == 5);
    checkRejected(run("--vita-shader-capture=yes"));
    // NEON decode: three-way renderer choice.
    assert(run("--vita-neon-decode=verify") == 7 && state.selections.at("neon_decode") == 2);
    assert(state.text.at("neon_decode_requested") == "verify" && state.gameArgs.size() == 5);
    checkRejected(run("--vita-neon-decode=fast"));
    // Fast pipeline lookup: three-way renderer choice.
    assert(run("--vita-pipeline-fast=verify") == 7 && state.selections.at("pipeline_fast") == 2);
    assert(state.text.at("pipeline_fast_requested") == "verify" && state.gameArgs.size() == 5);
    assert(run("") == 7 && state.text.at("pipeline_fast_requested") == "default off");
    for (const char* bad : {"--vita-pipeline-fast=1", "--vita-pipeline-fast=on --vita-pipeline-fast=off"})
        checkRejected(run(bad));
    // Exit timer: consumed by the port, never passed to the game.
    assert(run("--vita-exit-after=90 --stage R_SP01,4,2,0") == 7); checkClocks(true, false);
    assert(state.exitAfter == 90 && state.events.at("run_exit_after_requested_s") == 90);
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--stage", "R_SP01,4,2,0"}));
    for (const char* bad : {"--vita-exit-after", "--vita-exit-after=", "--vita-exit-after=9",
                            "--vita-exit-after=3601", "--vita-exit-after=90s", "--vita-exit-after=-5",
                            "--vita-exit-after=90 --vita-exit-after=90"}) checkRejected(run(bad));
    // A sequence file runs its lines in order, one per launch, then args.txt again.
    writeFile("ux0:data/twilight-vita/sequence.txt",
        "# T1\r\nhouse-off: --vita-clocks=system --stage R_SP01,4,2,0\r\n\n"
        "  outside-on:   --vita-exit-after=95 --stage F_SP103,1,3,-1  \n--spawn 2\n");
    std::filesystem::remove("ux0:data/twilight-vita/sequence_state.txt");
    assert(run("--vita-clocks=system --args-file-only") == 7); checkClocks(false, true);
    assert(state.text.at("sequence_label") == "house-off");
    assert(state.events.at("sequence_index") == 0 && state.events.at("sequence_count") == 3);
    assert(state.events.at("sequence_state_saved") == 1 && state.exitAfter == 0);
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--stage", "R_SP01,4,2,0"}));
    assert(run("--vita-clocks=system --args-file-only") == 7); checkClocks(true, false);
    assert(state.text.at("sequence_label") == "outside-on" && state.events.at("sequence_index") == 1);
    assert(state.exitAfter == 95);
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--stage", "F_SP103,1,3,-1"}));
    assert(run("--vita-clocks=system --args-file-only") == 7); checkClocks(true, false);
    assert(state.text.at("sequence_label").empty() && state.events.at("sequence_index") == 2);
    assert(std::vector<std::string>(state.gameArgs.begin() + 5, state.gameArgs.end()) ==
        (std::vector<std::string>{"--spawn", "2"}));
    assert(run("--vita-clocks=system --args-file-only") == 7); checkClocks(false, true);
    assert(state.events.at("sequence_complete") == 3 && !state.events.count("sequence_index"));
    assert(state.gameArgs.back() == "--args-file-only");
    writeFile("ux0:data/twilight-vita/sequence_state.txt", "garbage");
    assert(run("--vita-clocks=system") == 7); assert(state.events.at("sequence_index") == 0);
    writeFile("ux0:data/twilight-vita/sequence.txt", std::string(4097, '#'));
    checkRejected(run("--vita-clocks=system"));
    writeFile("ux0:data/twilight-vita/sequence.txt", std::string("x: ") + std::string(520, 'a'));
    writeFile("ux0:data/twilight-vita/sequence_state.txt", "0");
    checkRejected(run("--vita-clocks=system"));
    writeFile("ux0:data/twilight-vita/sequence.txt", "only: --vita-clocks=bad");
    writeFile("ux0:data/twilight-vita/sequence_state.txt", "0");
    checkRejected(run("--vita-clocks=system"));
    std::filesystem::remove("ux0:data/twilight-vita/sequence.txt");
    std::filesystem::remove("ux0:data/twilight-vita/sequence_state.txt");
    // Start-up screens (runtime/startup_check.hpp): the file the option came from, and the option.
    writeFile("ux0:data/twilight-vita/sequence.txt", "only: --vita-ticks=sometimes");
    writeFile("ux0:data/twilight-vita/sequence_state.txt", "0");
    checkRejected(run(nullptr));
    assert(state.text.at("screen_title") == "Problem in sequence.txt" &&
           state.text.at("screen_path") == "ux0:data/twilight-vita/sequence.txt");
    std::filesystem::remove("ux0:data/twilight-vita/sequence.txt");
    std::filesystem::remove("ux0:data/twilight-vita/sequence_state.txt");
    checkRejected(run("--stage R_SP01,4,2,0 --vita-ticks=sometimes"));
    assert(state.text.at("screen_title") == "Problem in args.txt" &&
           state.text.at("screen_path") == "ux0:data/twilight-vita/args.txt" &&
           state.text.at("screen_problem") == "--vita-ticks needs fixed, frames or single, once.");
    // The disc, checked before anything else: missing (disc-like files in the folder are named), too
    // short to read, not a disc, the other regions Dusklight accepts, another game, cut short.
    folder = {"notes.txt", "Zelda.iso", "game.iso.txt", "tp.rvz"};
    assert(run("--vita-clocks=system", false) == 2 && state.gameCalls == 0 && state.nativeCalls.empty());
    assert(state.text.at("startup_screen") == "disc_missing" && state.text.count("no_disc") &&
           state.text.at("disc_similar_files") == "Zelda.iso, game.iso.txt" &&
           state.text.at("screen_path") == "ux0:data/twilight-vita/game.iso");
    folder.clear();
    discImage = "host fixture";
    assert(run(nullptr) == 2 && state.text.at("startup_screen") == "disc_unreadable");
    discImage = std::string(0x3000, 'x');
    assert(run(nullptr) == 2 && state.text.at("startup_screen") == "disc_not_gamecube");
    discImage = gameCubeImage("GZ2E01");
    assert(run("--vita-clocks=system") == 7 && state.text.at("disc_id") == "GZ2E01" &&
           state.events.at("disc_tested_on_vita") == 0);
    discImage = gameCubeImage("GALE01");
    assert(run(nullptr) == 2 && state.text.at("startup_screen") == "disc_wrong_game");
    discImage = gameCubeImage("GZ2P01").substr(0, 0x2900); // ends inside its only file
    assert(run(nullptr) == 2 && state.text.at("startup_screen") == "disc_truncated" &&
           state.events.at("disc_needed_bytes") == 0x3000);
    discImage = gameCubeImage("GZ2P01");
    assert(run("--vita-clocks=system") == 7 && state.text.at("disc_check") == "ok" &&
           state.events.at("disc_tested_on_vita") == 1 && !state.text.count("startup_screen"));
#if TWV_VITA_BENCHMARK
    assert(run("--vita-clocks=legacy444 --vita-benchmark=C1-FIGHT") == 7);
    checkClocks(true, true); assert(state.benchmarkCalls == 1 && state.gameArgs.size() == 5);
    checkRejected(run("--vita-benchmark=C1-FIGHT --vita-clocks=legacy444 --vita-benchmark=C1-FIGHT"));
#else
    checkRejected(run("--vita-clocks=legacy444 --vita-benchmark=C1-FIGHT"));
#endif
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(root);
    std::puts("clock_entry_test: real entry preserves game/renderer CLI; validates port options before clocks; signed receipts and benchmark build variants passed");
}
