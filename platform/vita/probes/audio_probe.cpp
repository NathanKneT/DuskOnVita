// Audio output probe: runs platform/vita/audio (the SceAudioOut path DuskAudioSystem
// uses on the Vita) without the game, so the port, worker thread, pull/put FIFO and
// pause/resume can be checked in Vita3K or on hardware without a disc image.
// Plays a quiet 440 Hz tone for 3 s, pauses 0.5 s, resumes 1 s, then closes and
// writes the output statistics to runtime.jsonl. Built as twilight_vita_audio_probe.vpk
// with the game's title ID, so tools/vita3k_smoke.sh runs it unchanged.
#include "audio/audio_out.hpp"
#include "runtime/telemetry.hpp"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cmath>
#include <vector>

namespace {
// 80-sample chunks like Dusklight's DSP subframes, rendered until the request is met.
constexpr int kChunk = 80;
double gPhase = 0.0;
unsigned gPulls = 0;

void pull(int neededBytes) {
    ++gPulls;
    float chunk[kChunk * 2];
    for (int produced = 0; produced < neededBytes; produced += static_cast<int>(sizeof chunk)) {
        for (int i = 0; i < kChunk; ++i) {
            const float v = 0.1f * static_cast<float>(std::sin(gPhase));
            gPhase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
            chunk[i * 2] = v;
            chunk[i * 2 + 1] = v;
        }
        twv::vita::audio::put(chunk, sizeof chunk);
    }
}
}  // namespace

int main(int, char**) {
    using namespace twv::vita;
    telemetryInit();
    installFaultReceipts();
    telemetryText("probe", "audio_out");
    if (!audio::open(48000, 2, pull)) {
        telemetryEvent("audio_probe_open_failed", 1);
        telemetryFlush();
        sceKernelExitProcess(1);
        return 1;
    }
    audio::resume();
    sceKernelDelayThread(3000000);
    const auto playing = audio::stats();
    telemetryEvent("probe_buffers_after_3s", playing.buffers);
    audio::pause();
    sceKernelDelayThread(500000);
    const auto paused = audio::stats();
    telemetryEvent("probe_silent_buffers_while_paused", paused.silentBuffers - playing.silentBuffers);
    audio::resume();
    sceKernelDelayThread(1000000);
    telemetryEvent("probe_pulls", gPulls);
    audio::close(); // writes audio_buffers / audio_late_buffers / audio_max_render_us
    const auto final = audio::stats();
    telemetryEvent("probe_device_error", final.deviceError);
    telemetryEvent("probe_done", 0);
    telemetryFlush();
    sceKernelExitProcess(0);
    return 0;
}
