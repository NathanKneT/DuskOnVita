// T2 (patches/aurora/0007): the cores when the GX FIFO runs on its own worker. The game thread
// keeps core 0, the FIFO worker (and with it every vitaGL draw) takes core 1; the audio worker and
// the receipt writer stay on core 2. SDK domain; plain C types cross to the game side.
#include "telemetry.hpp"

#include <psp2/kernel/threadmgr.h>

namespace {
constexpr int kCore0 = 0x10000; // SCE_KERNEL_CPU_MASK_USER_0
constexpr int kCore1 = 0x20000; // SCE_KERNEL_CPU_MASK_USER_1
}

// Called by the worker itself, first thing (lib/gx/fifo.cpp, weak there).
extern "C" void twv_gx_worker_started() noexcept {
    const SceUID self = sceKernelGetThreadId();
    twv::vita::telemetryEvent("gx_worker_thread", self);
    twv::vita::telemetryEvent("gx_worker_affinity_result", sceKernelChangeThreadCpuAffinityMask(self, kCore1));
}

// main_vita.cpp, on the main thread, when --vita-gx-thread=on.
extern "C" int twv_vita_gx_thread_pin_main(void) {
    return sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), kCore0);
}
