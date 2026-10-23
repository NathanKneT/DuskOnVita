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
int run(const char* args, bool disc = true, bool bundle = true, bool renderer = true, int gameResult = 7) {
    state = {}; state.bundleAvailable = bundle; state.rendererAvailable = renderer;
    state.gameResult = gameResult;
    if (args) writeFile("ux0:data/twilight-vita/args.txt", args);
    else std::filesystem::remove("ux0:data/twilight-vita/args.txt");
    if (disc) writeFile("ux0:data/twilight-vita/game.iso", "host fixture");
    else std::filesystem::remove("ux0:data/twilight-vita/game.iso");
    // Existing entry behavior ignores the process argv; args.txt remains the
    // supported option source, even if a process argument resembles our option.
    char arg0[] = "host", ignored[] = "--vita-clocks=legacy444";
    char* argv[]{arg0, ignored, nullptr};
    return clockEntryUnderTest(2, argv);
}
void checkRejected(int result) {
    assert(result == 2 && state.gameCalls == 0 && state.exits == std::vector<int>{2});
    assert(state.nativeCalls.empty());
    assert(state.text.count("args_error"));
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
}
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
extern "C" int twv_vita_shader_capture_select(int value) { return selectRenderer("capture", value); }
extern "C" int twv_vita_neon_decode_select(int value) { return selectRenderer("neon_decode", value); }
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
    assert(run(nullptr) == 7); checkClocks(true, false); assert(state.gameArgs.size() == 5);
    assert(state.exitAfter == 0 && !state.events.count("sequence_index") && !state.events.count("sequence_complete"));
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
