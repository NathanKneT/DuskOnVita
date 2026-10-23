#include "gfx_vgl/frame_pacing.hpp"
#include "gfx_vgl/gpu_api.h"
#include "gfx_vgl/profile_scope.hpp"
#include "gfx_vgl/uniform_state.hpp"
#include "gfx_vgl/shader_stage_pool.hpp"
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

// Compile the production receipt function, too: stage averages use 30 frames,
// while wall-time averages use the actual number of intervals (first window29).
std::map<std::string,int64_t> receipts;
std::map<std::string,std::string> textReceipts;
std::vector<std::string> histogramOrder;
unsigned flushes=0;
namespace twv::vita {
void telemetryEvent(const char* key,int64_t value){receipts[key]=value;}
void telemetryText(const char* key,const char* value){
    assert(std::string(value).size()<=254);
    textReceipts[key]=value;histogramOrder.emplace_back(key);
}
void telemetryFlush(){++flushes;}
namespace audio {
struct Stats {uint64_t buffers=0,late=0,maxRenderUs=0;};
Stats stats(){return {};}
}
}
twv::vgl::FramePacing framePacing;
std::array<uint64_t,TWV_PROFILE_COUNT> profileUs{};
uint32_t programCacheHits=0,uniquePrograms=0;
twv::vgl::ShaderStagePool<> shaderStagePool;
uint64_t shaderStageRequests=0,shaderCompileCalls=0;
uint64_t directVertexBytes=0,directIndexBytes=0;
uint64_t samplerRequests=0,samplerUpdates=0,samplerSkips=0;
uint64_t uniformRequests=0,uniformUpdates=0,uniformSkips=0,uniformUploadBytes=0,uniformSkippedBytes=0;
twv::vgl::ProgramUniformCache programUniformCache;
uint64_t rasterRequests=0,rasterUpdates=0;
// This seam checks exporter cadence; sampler_unit_test executes the real
// sampler exporter and its values with the owned sampler helpers.
unsigned samplerReceiptReports=0;
void unit_sampler_receipts(){++samplerReceiptReports;}
#include "profile_receipts_function.inc"

int main() {
    using twv::vgl::FramePacing;using twv::vgl::FramePacingStats;
    FramePacing p;FramePacingStats s;uint64_t now=0;
    // Thirty boundaries have 29 intervals on the first window, even at time0.
    for(unsigned i=0;i<30;++i){assert(p.frame_end(now,s)==(i==29));now+=1000;}
    assert(s.frames==30 && s.intervals==29 && s.meanUs==1000 && s.p99Us==1000 && !s.rejected);
    assert(s.epoch==0 && s.sequence==1 && s.startUs==0 && s.endUs==29000 && s.histogram[1]==29);
    // Next window includes the boundary crossing. 1..30 ms: exact nearest ranks.
    now-=1000;
    for(unsigned i=1;i<=30;++i){now+=i*1000;assert(p.frame_end(now,s)==(i==30));}
    assert(s.intervals==30 && s.meanUs==15500 && s.minUs==1000 && s.p50Us==15000);
    assert(s.p95Us==29000 && s.p99Us==30000 && s.maxUs==30000 && !s.overBudget);
    assert(s.sequence==2 && s.startUs==29000 && s.endUs==494000);
    for(unsigned i=1;i<=30;++i){now+=i==23?100000:33334;assert(p.frame_end(now,s)==(i==30));}
    assert(s.intervals==30 && s.overBudget==1 && s.p50Us==33334 && s.p95Us==33334 && s.p99Us==100000);
    // A regressing clock yields no fabricated long interval. Reset drops the
    // prior session's boundary and the first window returns to 29 samples.
    p.reset();now=1000;
    for(unsigned i=0;i<30;++i){now=i%2?900:1000;assert(p.frame_end(now,s)==(i==29));}
    assert(s.intervals==14 && s.rejected==15 && s.meanUs==100 && !s.overBudget);
    assert(s.epoch==1 && s.sequence==1 && s.histogram[1]==14);
    p.reset();now=0;
    for(unsigned i=0;i<30;++i)assert(p.frame_end(now,s)==(i==29));
    assert(s.intervals==29 && !s.rejected && s.meanUs==0 && s.maxUs==0);
    assert(s.histogram[0]==29);
    // Huge valid intervals plus clock regressions must not overflow an average.
    p.reset();
    for(unsigned i=0;i<30;++i)assert(p.frame_end(i%2?UINT64_MAX:0,s)==(i==29));
    assert(s.intervals==15 && s.rejected==14 && s.meanUs==UINT64_MAX && s.maxUs==UINT64_MAX);
    assert(s.histogram.back()==15);
    // Every inclusive boundary and its first successor go in distinct bins.
    for(size_t bin=0;bin<twv::vgl::FrameIntervalUpperUs.size();++bin) {
        const auto upper=twv::vgl::FrameIntervalUpperUs[bin];
        p.reset();now=0;assert(!p.frame_end(now,s));
        for(unsigned i=0;i<29;++i){now+=upper;assert(p.frame_end(now,s)==(i==28));}
        assert(s.histogram[bin]==29);
        p.reset();now=0;assert(!p.frame_end(now,s));
        for(unsigned i=0;i<29;++i){now+=upper+1;assert(p.frame_end(now,s)==(i==28));}
        assert(s.histogram[bin+1]==29);
    }
    // A reset discards partial samples, including their starting boundary.
    p.reset();p.frame_end(100,s);p.frame_end(200,s);p.reset();
    for(unsigned i=0;i<30;++i)assert(p.frame_end(500+i*100,s)==(i==29));
    assert(s.sequence==1 && s.startUs==500 && s.intervals==29 && s.histogram[1]==29);
    // Compare thousands of windows against a wider independent exact sum.
    p.reset();p.frame_end(0,s);now=0;uint32_t rng=0x303334;
    std::array<uint64_t,30> times{};
    for(unsigned window=0;window<2000;++window) {
        const unsigned n=window?30:29;
        unsigned over=0;unsigned __int128 sum=0;
        for(unsigned i=0;i<n;++i) {
            rng=rng*1664525u+1013904223u;times[i]=rng%100001;
            sum+=times[i];over+=times[i]>33334;now+=times[i];
            assert(p.frame_end(now,s)==(i==n-1));
        }
        std::sort(times.begin(),times.begin()+n);
        assert(s.frames==30 && s.intervals==n && !s.rejected);
        assert(s.meanUs==sum/n && s.overBudget==over && s.minUs==times[0] && s.maxUs==times[n-1]);
        assert(s.p50Us==times[(n+1)/2-1] && s.p95Us==times[(95*n+99)/100-1] && s.p99Us==times[n-1]);
        uint32_t histogramTotal=0;
        for(size_t bin=0;bin<s.histogram.size();++bin) {
            unsigned expected=0;
            for(unsigned i=0;i<n;++i)
                expected+=(bin==0 || times[i]>twv::vgl::FrameIntervalUpperUs[bin-1]) &&
                    (bin==twv::vgl::FrameIntervalUpperUs.size() || times[i]<=twv::vgl::FrameIntervalUpperUs[bin]);
            assert(s.histogram[bin]==expected);histogramTotal+=s.histogram[bin];
        }
        assert(histogramTotal==s.intervals);
    }
    for(unsigned i=0;i<30;++i) {
        for(auto& stage:profileUs)stage+=100+i;
        profile_receipts(uint64_t(i)*33334);
        if(i<29)assert(receipts.empty() && !flushes && !samplerReceiptReports);
    }
    assert(flushes==1 && receipts.at("prof_frame_samples")==30 && receipts.at("prof_interval_samples")==29);
    assert(samplerReceiptReports==1);
    assert(receipts.at("prof_frame_interval_us")==33334 && !receipts.at("prof_frame_over_33334us"));
    assert(receipts.at("prof_frame_p95_us")==33334);
    assert(textReceipts.at("prof_interval_hist_begin")==
        "v=1;unit=us;e=0;s=1;f=30;a=29;r=0;t0=0;t1=966686;x=33334");
    assert(textReceipts.at("prof_interval_hist_bounds_us")==twv::vgl::FrameIntervalBoundsText);
    assert(textReceipts.at("prof_interval_hist_counts")=="0,0,0,0,0,29,0,0,0,0,0,0,0,0,0,0,0");
    assert(textReceipts.at("prof_interval_hist_end")=="e=0;s=1");
    assert((histogramOrder==std::vector<std::string>{"prof_interval_hist_begin","prof_interval_hist_bounds_us",
        "prof_interval_hist_counts","prof_interval_hist_end"}));
    assert(receipts.at("prof_draw_timers_enabled")==TWV_VITA_DRAW_TIMERS);
    assert(receipts.at("prof_shader_us")==114 && receipts.at("prof_fifo_inclusive_us")==114);
    for(const auto* key:{"prof_vertex_cpu_us","prof_gl_submit_us","prof_state_us","prof_pipeline_us"}) {
        if(TWV_VITA_DRAW_TIMERS)assert(receipts.at(key)==114);
        else assert(!receipts.count(key));
    }
    for(auto stage:profileUs)assert(!stage);
    for(unsigned i=30;i<60;++i) {
        for(auto& stage:profileUs)stage+=1000;
        profile_receipts(uint64_t(i)*33334);
    }
    assert(flushes==2 && receipts.at("prof_interval_samples")==30 && receipts.at("prof_shader_us")==1000);
    assert(samplerReceiptReports==2);
    if(TWV_VITA_DRAW_TIMERS)assert(receipts.at("prof_vertex_cpu_us")==1000);
    else assert(!receipts.count("prof_vertex_cpu_us"));
    assert(receipts.at("prof_frame_interval_us")==33334 && !receipts.at("prof_interval_rejected"));
    assert(textReceipts.at("prof_interval_hist_begin")==
        "v=1;unit=us;e=0;s=2;f=30;a=30;r=0;t0=966686;t1=1966706;x=33334");
    // The real exporter preserves UINT64_MAX without EventLog's signed cast.
    framePacing.reset();
    for(unsigned i=0;i<30;++i)profile_receipts(i%2?UINT64_MAX:0);
    assert(samplerReceiptReports==3);
    assert(textReceipts.at("prof_interval_hist_begin").find("x=18446744073709551615")!=std::string::npos);
    assert(textReceipts.at("prof_interval_hist_counts")=="0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,15");
    std::printf("frame_pacing_test: production receipts, first29/subsequent30 intervals, exact ranks, stalls, zero/regressing/huge clocks and2000 windows passed; %zu bytes\n",sizeof p);
}
