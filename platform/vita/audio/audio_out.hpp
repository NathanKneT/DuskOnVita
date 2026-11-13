#pragma once
// SceAudioOut output for Dusklight's DSP emulation (src/dusk/audio/DuskAudioSystem.cpp,
// patched for TARGET_VITA). SDK-free header with plain types only, so it can be
// included from int-enum code (docs/ARCHITECTURE.md, "Enum ABI").
//
// Pull model, as the SDL audio stream it replaces: a kernel worker thread owns the
// MAIN port (48 kHz, stereo, S16) and, whenever its FIFO holds less than one device
// buffer, calls the registered pull function with the number of F32 bytes it still
// needs. The pull function renders audio frames and hands them to put() (on the same
// thread), exactly as GetNewAudio does with SDL_PutAudioStreamData upstream.
#include <cstddef>
#include <cstdint>

namespace twv::vita::audio {

using PullFn = void (*)(int neededBytes);

struct Stats {
    std::uint32_t buffers = 0;       // device buffers submitted
    std::uint32_t late = 0;          // buffers whose render took longer than their playback time
    std::uint32_t maxRenderUs = 0;   // slowest buffer render
    std::uint32_t silentBuffers = 0; // buffers output as silence while paused
    std::int32_t deviceError = 0;    // last negative SDK result, 0 if none
    // Wall time and the worker thread's own run clocks (kernel units) over every rendered buffer,
    // and over the late ones alone. A late buffer with few run clocks for its wall time waited
    // (another thread had core 2, or a lock was held); one with the usual rate was real work.
    std::uint64_t renderWallUs = 0, renderRunClocks = 0, lateWallUs = 0, lateRunClocks = 0;
    // How the rendering is chunked: calls of the pull function, the frames it handed to put(),
    // rendered buffers that needed no pull, and the buffers' render wall time by bin
    // (under 2, 4, 6, 8 ms, under one buffer period of 10.67 ms, under 13, 16 ms, and above).
    std::uint32_t pulls = 0, buffersWithoutPull = 0;
    std::uint64_t putFrames = 0;
    std::uint32_t renderHist[8] = {};
    // patches/dusklight/0071 (measurement only): the rendered subframes (80 samples each), the
    // voices active summed over them, and their time in the game's sound driver, in the mixer and
    // in the rest (stream mix, interleave, hand-over).
    std::uint64_t subframes = 0, voiceSubframes = 0, driverUs = 0, mixUs = 0, restUs = 0;
    // patches/dusklight/0072 (measurement only): the mixer's subframes that were timed by stage
    // (one in sixteen), their time in a voice's decode/resampling/filter, volume and panning,
    // reverb input and surround, mix, and after the voices (reverb and the rest); the voices in
    // them, those with the biquad filter, and the input samples their pitch asked for.
    std::uint64_t mixTimed = 0, mixStageUs[5] = {}, mixVoices = 0, mixBiquads = 0, mixInput = 0;
    std::uint32_t fpscr = 0; // the sound thread's floating-point control register at its start
};

// "--vita-audio-priority=default|high" (before open()): high puts the worker 16 steps above the
// default user priority, so that loaders and other threads that land on core 2 cannot hold it
// while a buffer is due. Default keeps the priority every console run so far has used.
void setHighPriority(bool on);
bool highPriority();

// "--vita-audio-pull=frame|subframe" (before open(); patches/dusklight/0067): how much the pull
// function renders per call. frame (the default, as upstream): a whole sound frame, 800 samples
// at 48 kHz, so two device buffers in three carry a frame's work and the third none. subframe:
// 80 samples per call, the same subframes in the same order, spread evenly over the buffers.
void setSubframePull(bool on);
bool subframePull();

// Opens the port and starts the worker, paused (silence) until resume().
// Only 48000 Hz stereo is accepted (the MAIN port's fixed rate; the Vita has stereo output).
bool open(int sampleRate, int channels, PullFn pull);
// Appends interleaved F32 frames; only valid from inside the pull function.
void put(const float* interleaved, std::size_t bytes);
void resume();
void pause();
// Stops and joins the worker, then releases the port. Safe to call when not open.
void close();
bool isOpen();
Stats stats();
// Main-thread diagnostic only: actual worker UID, negative when unavailable.
std::int32_t threadId();

}  // namespace twv::vita::audio
