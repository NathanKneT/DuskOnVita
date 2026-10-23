#pragma once
// SDK domain, shared by every GPU backend: timing categories, frame pacing, CPU heap
// budgets, memory/log receipts and the run-control frame hook. Moved out of the
// vitaGL file in G0 so the GXM backend emits the same receipts.
#include "gpu_backend.h"
#include <cstdint>

namespace twv::gpu {
void common_init();
// After each presented frame, in the order the vitaGL path always used: pacing
// window (with backend.pacing_report), system probe, run-control exit timer, then
// every 60 frames backend.report() and the shared 60-frame receipts.
void common_frame(const TwvGpuBackend& backend);
uint32_t frames_presented();
struct CpuHeapUsage {uint32_t valid=0,used=0,arena=0;};
CpuHeapUsage cpu_heap_usage();
}
