// SDK side of --vita-exit-after (run_control.hpp).
#include "run_control.hpp"
#include "telemetry.hpp"

#include <psp2/kernel/processmgr.h>

#include <atomic>
#include <cstdint>

namespace {
unsigned gExitAfterS = 0;
std::atomic<int> gPlayerSeen{0};
std::uint64_t gPlayerUs = 0;
constexpr std::uint64_t NoPlayerGraceS = 240;
}

namespace twv::vita {
void runControlExitAfter(unsigned seconds) { gExitAfterS = seconds; }
bool runControlPlayerSeen() { return gPlayerSeen.load(std::memory_order_relaxed) != 0; }

void runControlFrame() {
    if (!gExitAfterS) return;
    const std::uint64_t now = processTimeUs();
    if (!gPlayerUs && gPlayerSeen.load(std::memory_order_relaxed)) {
        gPlayerUs = now ? now : 1;
        telemetryEvent("run_player_first_us", static_cast<long long>(gPlayerUs));
    }
    const std::uint64_t limit = std::uint64_t(gExitAfterS) * 1000000ull;
    const bool due = gPlayerUs ? now - gPlayerUs >= limit : now >= limit + NoPlayerGraceS * 1000000ull;
    if (!due) return;
    telemetryEvent("run_exit_after_s", gExitAfterS);
    telemetryEvent("run_exit_player_seen", gPlayerUs != 0);
    telemetryEvent("run_exit_us", static_cast<long long>(now));
    telemetrySync();
    sceKernelExitProcess(0);
}
}

extern "C" void twv_vita_run_player_seen(int present) {
    if (present) gPlayerSeen.store(1, std::memory_order_relaxed);
}
