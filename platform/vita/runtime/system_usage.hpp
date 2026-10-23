#pragma once
#include <cstdint>
namespace twv::vita {
// Native counter units are deliberately preserved; no CPU/GPU percentage inferred.
enum class UsageState : int {Unavailable=0,Baseline=1,Delta=2,CounterResetOrWrap=3,TimeReset=4};
struct UsageDelta {UsageState state=UsageState::Unavailable;uint64_t clocks=0,elapsedUs=0;};
class UsageCounter {
    uint64_t previous=0,time=0,identity=0;
    bool ready=false;
public:
    UsageDelta sample(uint64_t now,uint64_t value,bool valid,uint64_t id=0) {
        if(!valid){ready=false;return {};}
        UsageDelta result;
        if(!ready || id!=identity)result.state=UsageState::Baseline;
        else if(now<=time)result.state=UsageState::TimeReset;
        else if(value<previous)result.state=UsageState::CounterResetOrWrap;
        else result={UsageState::Delta,value-previous,now-time};
        ready=true;previous=value;time=now;identity=id;
        return result;
    }
};
class UsageSampleGate {
    bool ready=false;
    uint64_t last=0;
public:
    bool due(uint64_t now) {
        if(ready && now>=last && now-last<1000000)return false;
        ready=true;last=now;return true;
    }
};
}
