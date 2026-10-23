#include "../platform/vita/runtime/system_usage.hpp"
#include <cassert>
#include <cstdint>
using namespace twv::vita;
int main() {
    UsageSampleGate gate;
    assert(gate.due(0));assert(!gate.due(999999));assert(gate.due(1000000));
    assert(!gate.due(1000000));assert(gate.due(1)); // Timestamp reset re-baselines.
    assert(gate.due(UINT64_MAX));assert(gate.due(0));
    UsageCounter c;
    auto d=c.sample(0,100,true);assert(d.state==UsageState::Baseline);
    d=c.sample(1000000,150,true);assert(d.state==UsageState::Delta && d.clocks==50 && d.elapsedUs==1000000);
    d=c.sample(2000000,150,true);assert(d.state==UsageState::Delta && d.clocks==0);
    d=c.sample(3000000,1,true);assert(d.state==UsageState::CounterResetOrWrap);
    d=c.sample(3000000,2,true);assert(d.state==UsageState::TimeReset);
    d=c.sample(1,3,true);assert(d.state==UsageState::TimeReset);
    d=c.sample(2,4,false);assert(d.state==UsageState::Unavailable);
    d=c.sample(3,5,true);assert(d.state==UsageState::Baseline);
    d=c.sample(4,6,true,42);assert(d.state==UsageState::Baseline); // Thread identity replaced.
    d=c.sample(5,7,true,42);assert(d.state==UsageState::Delta && d.clocks==1);
    UsageCounter large;
    assert(large.sample(0,0,true).state==UsageState::Baseline);
    d=large.sample(UINT64_MAX,UINT64_MAX,true);
    assert(d.state==UsageState::Delta && d.clocks==UINT64_MAX && d.elapsedUs==UINT64_MAX);
    d=large.sample(1,1,true);assert(d.state==UsageState::TimeReset);
    UsageCounter wrap;
    wrap.sample(1,UINT64_MAX,true);
    assert(wrap.sample(2,0,true).state==UsageState::CounterResetOrWrap);
    // Conservatively invalidate32-bit kernel counter rollover as well as reset.
    wrap.sample(3,UINT32_MAX,true);
    assert(wrap.sample(4,0,true).state==UsageState::CounterResetOrWrap);
}
