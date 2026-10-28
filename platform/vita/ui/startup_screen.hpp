#pragma once
// Start-up screens on the Vita display (startup_screen.cpp; drawing in startup_canvas.hpp, texts in
// runtime/startup_check.hpp). The CPU draws into a 960x544 A8B8G8R8 buffer and sceDisplaySetFrameBuf
// shows it directly: no GXM scene, no shader, so the error screens work when the shader compiler is
// missing or GXM could not start, and the progress screen works before the game draws its first frame.
// Plain types only (enum class with a fixed underlying type), callable from either enum domain.
#include "runtime/startup_check.hpp"

#include <cstdint>

namespace twv::vita {

// Shows the message, writes receipts (startup_screen, startup_screen_title, startup_screen_display),
// waits until every button is released and one is pressed (startup_screen_buttons), then ends the
// process with `code`. Without a usable display buffer it exits at once (the receipts say so).
[[noreturn]] void startupErrorExit(const MessageText& text, int code);

// The GXM backend, while it prepares the shader warm-up bundle. `buffer`: one of its display
// buffers, at least 960 x 544 x 4 bytes, CPU-writable, not queued for display yet.
// Begin draws "Loading..." and shows the buffer. Update is cheap to call after every stage: it draws
// only when `compiling` (a stage was compiled, not loaded from the cache: the slow first launch),
// the first time with the whole progress screen and later the bar and count, at most every 100 ms.
// End draws "Starting the game..." over a shown progress screen; the buffer stays on the display
// until the backend's first frame flips. Receipts: startup_progress_* (shown, draws, draw_us).
void startupProgressBegin(void* buffer, uint32_t total);
void startupProgressUpdate(uint32_t done, uint32_t total, bool compiling);
void startupProgressEnd(uint32_t done, uint32_t total);

// A graphics backend notes why its initialisation is failing (the shader compiler is missing, a
// memory block could not be reserved, or another step: `detail`); the failure path then shows it
// with startupGraphicsFailureExit.
void startupNoteProblem(StartupProblem problem, const char* detail, uint64_t have, uint64_t need);
[[noreturn]] void startupGraphicsFailureExit(int code);

}
