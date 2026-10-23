#pragma once
// End-of-run control for console measurement runs (--vita-exit-after). The
// process exits cleanly N seconds after the player first appears, or N + 240 s
// after boot if the player never appears. Receipts are drained first.
namespace twv::vita {
void runControlExitAfter(unsigned seconds);
// Once per presented frame, on the render thread.
void runControlFrame();
// Whether the scene probe has seen the player (any thread).
bool runControlPlayerSeen();
}
// Game side (scene probe), plain int across the enum-ABI boundary.
extern "C" void twv_vita_run_player_seen(int present);
