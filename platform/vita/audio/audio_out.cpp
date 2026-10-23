// SceAudioOut side of the Vita audio output (audio_out.hpp). SDK domain: built with
// the toolchain's short enums and exchanging only plain types with Dusklight.
//
// Design verified on a Vita 1000 in an earlier Vita project by the same author: one kernel
// worker thread owns the port and blocks in
// sceAudioOutOutput, which returns when the previous buffer has been consumed, so
// two device buffers alternate. Differences: 48 kHz on the MAIN port (Dusklight's
// DSP rate), and pull rendering of F32 frames that are converted to S16 here.
// SDK out-parameter rule (from a data abort on a Vita 1000): every pointer handed to the SDK
// points at word-sized storage (volume array, thread argument, wait timeout).
#include "audio_out.hpp"
#include "runtime/telemetry.hpp"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace twv::vita::audio {
namespace {

static_assert(sizeof(int) == 4 && sizeof(SceUInt) == 4, "volume words and wait timeout");

constexpr int kFrames = 512;    // per device buffer: 10.7 ms at 48 kHz (a multiple of 64, as the port requires)
constexpr int kChannels = 2;
constexpr SceUInt kJoinTimeoutUs = 2000000;
// Core 2, below the game's threads in importance but above loaders (PORT_DESIGN 3.3).
constexpr int kPriority = 0x10000100;
constexpr int kAffinity = 0x40000; // SCE_KERNEL_CPU_MASK_USER_2
constexpr int kStackSize = 256 * 1024; // the DSP render (64 voices, reverb) runs on this thread

struct State {
    int port = -1;
    SceUID thread = -1;
    PullFn pull = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{true};
    // FIFO of interleaved F32 samples, touched only by the worker (put() runs inside pull()).
    std::vector<float> fifo;
    std::size_t fifoRead = 0;
    alignas(64) std::int16_t device[2][kFrames * kChannels]{};
    std::atomic<std::uint32_t> buffers{0}, late{0}, maxRenderUs{0}, silent{0};
    std::atomic<std::int32_t> deviceError{0};
};
State gState;

std::int16_t to_s16(float v) {
    if (!(v == v)) return 0; // NaN
    const float scaled = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
    return static_cast<std::int16_t>(std::lrintf(scaled));
}

// Fills one device buffer from the FIFO, pulling more frames first if needed.
void render(std::int16_t* out) {
    constexpr std::size_t want = kFrames * kChannels;
    while (gState.fifo.size() - gState.fifoRead < want) {
        const std::size_t have = gState.fifo.size() - gState.fifoRead;
        const std::size_t before = gState.fifo.size();
        gState.pull(static_cast<int>((want - have) * sizeof(float)));
        if (gState.fifo.size() == before) {
            // The renderer produced nothing: pad with silence rather than spin.
            gState.fifo.resize(gState.fifoRead + want, 0.0f);
        }
    }
    const float* in = gState.fifo.data() + gState.fifoRead;
    for (std::size_t i = 0; i < want; ++i) {
        out[i] = to_s16(in[i]);
    }
    gState.fifoRead += want;
    if (gState.fifoRead == gState.fifo.size()) {
        gState.fifo.clear();
        gState.fifoRead = 0;
    } else if (gState.fifoRead > 16 * want) {
        gState.fifo.erase(gState.fifo.begin(), gState.fifo.begin() + static_cast<std::ptrdiff_t>(gState.fifoRead));
        gState.fifoRead = 0;
    }
}

int worker(SceSize, void*) {
    constexpr std::uint64_t bufferUs = 1000000ull * kFrames / 48000;
    int which = 0;
    while (gState.running.load(std::memory_order_acquire)) {
        std::int16_t* out = gState.device[which];
        if (gState.paused.load(std::memory_order_acquire)) {
            std::memset(out, 0, sizeof gState.device[which]);
            gState.silent.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::uint64_t start = processTimeUs();
            render(out);
            const auto took = static_cast<std::uint32_t>(processTimeUs() - start);
            if (took > gState.maxRenderUs.load(std::memory_order_relaxed)) {
                gState.maxRenderUs.store(took, std::memory_order_relaxed);
            }
            if (took > bufferUs) {
                gState.late.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const int rc = sceAudioOutOutput(gState.port, out);
        if (rc < 0) {
            gState.deviceError.store(rc, std::memory_order_relaxed);
            break;
        }
        gState.buffers.fetch_add(1, std::memory_order_relaxed);
        which ^= 1;
    }
    return 0;
}

}  // namespace

bool open(int sampleRate, int channels, PullFn pull) {
    if (gState.port >= 0 || pull == nullptr || sampleRate != 48000 || channels != kChannels) {
        return false;
    }
    const int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, kFrames, sampleRate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        telemetryEvent("audio_open_error", port);
        return false;
    }
    int volume[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL}; // master volume is applied by the DSP
    sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         volume);
    const SceUID thread = sceKernelCreateThread("twv_audio_out", worker, kPriority, kStackSize, 0, kAffinity, nullptr);
    if (thread < 0) {
        telemetryEvent("audio_thread_error", thread);
        sceAudioOutReleasePort(port);
        return false;
    }
    gState.port = port;
    gState.thread = thread;
    gState.pull = pull;
    gState.fifo.clear();
    gState.fifo.reserve(kFrames * kChannels * 8);
    gState.fifoRead = 0;
    gState.paused.store(true, std::memory_order_release);
    gState.running.store(true, std::memory_order_release);
    if (sceKernelStartThread(thread, 0, nullptr) < 0) {
        gState.running.store(false, std::memory_order_release);
        sceKernelDeleteThread(thread);
        sceAudioOutReleasePort(port);
        gState.port = -1;
        gState.thread = -1;
        return false;
    }
    telemetryEvent("audio_open_hz", sampleRate);
    return true;
}

void put(const float* interleaved, std::size_t bytes) {
    const std::size_t count = bytes / sizeof(float);
    gState.fifo.insert(gState.fifo.end(), interleaved, interleaved + count);
}

void resume() { gState.paused.store(false, std::memory_order_release); }
void pause() { gState.paused.store(true, std::memory_order_release); }
bool isOpen() { return gState.port >= 0; }

void close() {
    if (gState.port < 0) {
        return;
    }
    gState.running.store(false, std::memory_order_release);
    SceUInt timeout = kJoinTimeoutUs;
    if (sceKernelWaitThreadEnd(gState.thread, nullptr, &timeout) < 0) {
        // Stuck in a device call: leave the thread and port to process exit rather
        // than free state it may still touch.
        telemetryEvent("audio_close_timeout", 1);
        return;
    }
    sceKernelDeleteThread(gState.thread);
    sceAudioOutOutput(gState.port, nullptr); // waits for the buffer in flight
    sceAudioOutReleasePort(gState.port);
    gState.port = -1;
    gState.thread = -1;
    gState.pull = nullptr;
    const Stats s = stats();
    telemetryEvent("audio_buffers", s.buffers);
    telemetryEvent("audio_late_buffers", s.late);
    telemetryEvent("audio_max_render_us", s.maxRenderUs);
}

Stats stats() {
    Stats s;
    s.buffers = gState.buffers.load(std::memory_order_relaxed);
    s.late = gState.late.load(std::memory_order_relaxed);
    s.maxRenderUs = gState.maxRenderUs.load(std::memory_order_relaxed);
    s.silentBuffers = gState.silent.load(std::memory_order_relaxed);
    s.deviceError = gState.deviceError.load(std::memory_order_relaxed);
    return s;
}
std::int32_t threadId() { return gState.thread; }

}  // namespace twv::vita::audio
