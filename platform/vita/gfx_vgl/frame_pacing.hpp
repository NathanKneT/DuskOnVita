#pragma once
// End-of-frame wall time, including game work and display backpressure between
// boundaries. Fixed storage; no FPS/fidelity verdict and no overlapping sums.
#include <algorithm>
#include <array>
#include <cstdint>

namespace twv::vgl {
// Version 1: integer microseconds; each upper bound is inclusive. The final
// counter covers everything above the last bound, including UINT64_MAX.
inline constexpr std::array<uint64_t,16> FrameIntervalUpperUs{{
    0,8334,16667,25000,33333,33334,41667,50000,66667,83334,100000,
    125000,166667,250000,500000,1000000}};
inline constexpr char FrameIntervalBoundsText[]=
    "0,8334,16667,25000,33333,33334,41667,50000,66667,83334,100000,125000,166667,250000,500000,1000000;overflow";
struct FramePacingStats {
    uint32_t frames=0,intervals=0,rejected=0,overBudget=0;
    uint64_t meanUs=0,minUs=0,p50Us=0,p95Us=0,p99Us=0,maxUs=0;
    // Disjoint report windows, not cumulative counters. The first window after
    // reset has 29 interval attempts; later windows include the joining edge.
    uint64_t epoch=0,sequence=0,startUs=0,endUs=0;
    std::array<uint32_t,FrameIntervalUpperUs.size()+1> histogram{};
};
class FramePacing {
public:
    static constexpr uint32_t Window=30;
    // A 30 Hz period rounded upward to the timestamp's whole microsecond.
    static constexpr uint64_t BudgetUs=33334;
    bool frame_end(uint64_t now,FramePacingStats& out) noexcept {
        if(!frames)start=hasLast?last:now;
        if(hasLast) {
            if(now>=last)intervals[count++]=now-last;
            else ++rejected; // Never turn a regressing clock into an enormous gap.
        }
        last=now;hasLast=true;
        if(++frames<Window)return false;
        out={};out.frames=frames;out.intervals=count;out.rejected=rejected;
        // Saturation cannot wrap an identity into an earlier report. Consumers
        // reject a repeated identity rather than silently count it twice.
        if(sequence<UINT64_MAX)++sequence;
        out.epoch=epoch;out.sequence=sequence;out.startUs=start;out.endUs=now;
        if(count) {
            uint64_t sum=0;bool overflow=false;
            for(unsigned i=0;i<count;++i) {
                const auto bin=std::lower_bound(FrameIntervalUpperUs.begin(),
                    FrameIntervalUpperUs.end(),intervals[i])-FrameIntervalUpperUs.begin();
                ++out.histogram[size_t(bin)];
                if(intervals[i]>UINT64_MAX-sum)overflow=true;
                else sum+=intervals[i];
                if(intervals[i]>BudgetUs)++out.overBudget;
            }
            if(!overflow)out.meanUs=sum/count;
            else {
                // Rare overflow fallback; ordinary windows need one division.
                uint64_t quotient=0,remainder=0;
                for(unsigned i=0;i<count;++i) {
                    quotient+=intervals[i]/count;remainder+=intervals[i]%count;
                }
                out.meanUs=quotient+remainder/count;
            }
            std::sort(intervals.begin(),intervals.begin()+count);
            const auto rank=[&](unsigned percent){return intervals[(percent*count+99)/100-1];};
            out.minUs=intervals[0];out.maxUs=intervals[count-1];
            out.p50Us=rank(50);out.p95Us=rank(95);out.p99Us=rank(99);
        }
        frames=count=rejected=0; // Keep the boundary joining adjacent windows.
        return true;
    }
    // A reset drops a partial window and the joining edge. Its new epoch makes
    // that discontinuity explicit to an exported-histogram consumer.
    void reset() noexcept {
        frames=count=rejected=0;last=start=sequence=0;hasLast=false;
        if(epoch<UINT64_MAX)++epoch;
    }
private:
    std::array<uint64_t,Window> intervals{};
    uint64_t last=0,start=0,epoch=0,sequence=0;
    uint32_t frames=0,count=0,rejected=0;
    bool hasLast=false;
};
static_assert(sizeof(FramePacing)<320);
} // namespace twv::vgl
