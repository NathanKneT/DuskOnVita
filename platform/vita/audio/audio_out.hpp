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
};

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
