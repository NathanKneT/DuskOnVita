#pragma once
#include <cstdint>
namespace twv::vita {
// SDK-domain implementation. Call only on the serialized main/render thread,
// after FIFO drain. At most one sample/second, no catch-up loop, no new thread.
// Pass actual main and audio UIDs; nonpositive UID means unavailable (never SELF).
void system_probe_sample(int32_t mainThreadId,int32_t audioThreadId);
// Telemetry sys_*: *_result is native return (negative errors preserved).
// *_state:0 unavailable,1 baseline,2 valid delta,3 counter decrease/reset/wrap,
// 4 non-increasing timestamp. Deltas are omitted unless state2; raw uint64
// counters/deltas use decimal text so signed EventLog cannot corrupt high bits.
// Counter units remain native clocks: pinned headers do not establish a safe
// conversion to microseconds. No CPU utilization or GPU utilization is emitted.
// Per-core idle counters are system-wide, not process/thread attribution.
// Access to system info may be rejected by firmware; failures are logged, not
// replaced with zero. Affinity is a mask, current/last CPU are snapshots.
// MHz clock getters describe configured frequency, never utilization.
// At most83 event/text records per due sample; uses buffered telemetry without
// forced flush. Kernel free-memory values are pool capacity, not newlib heap use.
}
