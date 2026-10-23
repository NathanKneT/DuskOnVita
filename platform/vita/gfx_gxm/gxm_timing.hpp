#pragma once
// GPU timing diagnostic (--vita-gxm-gpu-timing=on): what one frame's CPU-observed times say
// about the GPU and the display. No SDK headers; host-tested in tests/gxm_timing_test.cpp.
//
// Times are process microseconds:
// - submit[k]: sceGxmEndScene of scene k;
// - fragmentDone[k]: when a monitor thread saw that scene's fragment notification;
// - vertexDone: the last scene's vertex notification;
// - presentStart/presentEnd: sceGxmDisplayQueueAddEntry (it blocks while two flips are pending);
// - flipStart/flipEnd: the display callback, entered once the GPU finished the frame,
//   left after its vblank wait (the frame is on screen from then).
// Retail GXM gives no GPU timestamps, so a scene's GPU time is the stretch from the moment
// it could start (submitted, and the previous scene done) to its fragment notification.
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace twv::gxm {
constexpr unsigned MaxTimedScenes=8; // the render target's scenesPerFrame

// What one scene of a split frame drew (--vita-gxm-gpu-split): the receipts relate its GPU time
// to blending, alpha test (discard, which defeats hidden-surface removal), depth-less 2D work
// and fragment program size.
struct SegmentStats {
    uint64_t draws=0,triangles=0,blended=0,discard=0,noDepth=0,fragmentBytes=0;
    void add(const SegmentStats& o) noexcept {
        draws+=o.draws;triangles+=o.triangles;blended+=o.blended;discard+=o.discard;noDepth+=o.noDepth;
        fragmentBytes+=o.fragmentBytes;
    }
};
// Triangles a GXM draw rasterises (count: indices, or vertices for the native primitives).
constexpr uint64_t triangles(uint32_t primitive,uint64_t count) noexcept {
    return primitive==1||primitive==2?(count>2?count-2:0):primitive==3?count/4*2:count/3;
}
// Draw boundaries for splitting a frame into `scenes` scenes, from the previous frame's draw
// count: scene k starts at draw ceil(k*draws/scenes). Fewer draws than scenes: no split.
inline unsigned split_points(uint64_t previousDraws,unsigned scenes,uint64_t* starts) noexcept {
    if(scenes<2 || scenes>MaxTimedScenes || previousDraws<scenes)return 1;
    for(unsigned k=1;k<scenes;++k)starts[k]=(k*previousDraws+scenes-1)/scenes;
    return scenes;
}

struct FrameTimes {
    uint32_t scenes=1;
    uint64_t begin=0,fenceWait=0; // fenceWait: gxm_begin's wait for the frame three back
    uint64_t submit[MaxTimedScenes]{},fragmentDone[MaxTimedScenes]{};
    uint64_t vertexDone=0,presentStart=0,presentEnd=0,flipStart=0,flipEnd=0;
    bool complete() const noexcept {
        if(!scenes || scenes>MaxTimedScenes || !flipEnd)return false;
        for(unsigned k=0;k<scenes;++k)if(!submit[k] || !fragmentDone[k])return false;
        return true;
    }
    uint64_t done() const noexcept {return fragmentDone[scenes-1];}
};

struct FrameMetrics {
    uint64_t gpuBusy=0;      // sum of the scenes' GPU stretches
    uint64_t gpuLatency=0;   // last submit to its fragment notification
    uint64_t gpuStarved=0;   // the GPU had finished everything and waited for the next submit
    uint64_t vertexTail=0;   // last submit to its vertex notification
    uint64_t presentWait=0;  // time in AddEntry
    uint64_t presentGpu=0;   // the part of it before the frame two back left the GPU
    uint64_t presentDisplay=0; // the rest: the display queue and vblank
    uint64_t flipInterval=0; // from the previous frame's flip (0 for the first)
    uint64_t displayLag=0;   // GPU done to the display callback
    uint64_t fenceWait=0;
    uint64_t segmentBusy[MaxTimedScenes]{};
};

inline uint64_t after(uint64_t later,uint64_t earlier) noexcept {return later>earlier?later-earlier:0;}

// previousDone: the previous frame's last fragment notification; olderDone: the frame two back
// (the flip AddEntry waits for); previousFlipEnd: the previous frame's flip. 0 = unknown.
inline FrameMetrics frame_metrics(const FrameTimes& f,uint64_t previousDone,uint64_t olderDone,
                                  uint64_t previousFlipEnd) noexcept {
    FrameMetrics m;
    uint64_t ready=previousDone;
    for(unsigned k=0;k<f.scenes;++k) {
        if(ready)m.gpuStarved+=after(f.submit[k],ready);
        const uint64_t start=std::max(f.submit[k],ready);
        m.segmentBusy[k]=after(f.fragmentDone[k],start);
        m.gpuBusy+=m.segmentBusy[k];
        ready=std::max(ready,f.fragmentDone[k]);
    }
    const uint64_t lastSubmit=f.submit[f.scenes-1];
    m.gpuLatency=after(f.done(),lastSubmit);
    m.vertexTail=f.vertexDone?after(f.vertexDone,lastSubmit):0;
    m.presentWait=after(f.presentEnd,f.presentStart);
    m.presentGpu=olderDone?std::min(after(olderDone,f.presentStart),m.presentWait):0;
    m.presentDisplay=m.presentWait-m.presentGpu;
    m.flipInterval=previousFlipEnd?after(f.flipEnd,previousFlipEnd):0;
    m.displayLag=f.flipStart?after(f.flipStart,f.done()):0;
    m.fenceWait=f.fenceWait;
    return m;
}

// One receipt window (the 30-frame pacing report): sums, a count and two maxima.
struct TimingWindow {
    uint64_t frames=0,flips=0,maxGpuBusy=0,maxPresentWait=0;
    FrameMetrics sum;
    uint32_t scenes=0; // scene count of the frames summed (0 if they differed)
    bool mixedScenes=false;
    SegmentStats segments[MaxTimedScenes]; // sums over the frames
    void add(const FrameMetrics& m,uint32_t frameScenes,const SegmentStats* stats=nullptr) noexcept {
        if(stats)for(unsigned k=0;k<frameScenes && k<MaxTimedScenes;++k)segments[k].add(stats[k]);
        if(!frames)scenes=frameScenes;else if(scenes!=frameScenes)mixedScenes=true;
        ++frames;if(m.flipInterval)++flips;
        maxGpuBusy=std::max(maxGpuBusy,m.gpuBusy);maxPresentWait=std::max(maxPresentWait,m.presentWait);
        sum.gpuBusy+=m.gpuBusy;sum.gpuLatency+=m.gpuLatency;sum.gpuStarved+=m.gpuStarved;
        sum.vertexTail+=m.vertexTail;sum.presentWait+=m.presentWait;sum.presentGpu+=m.presentGpu;
        sum.presentDisplay+=m.presentDisplay;sum.flipInterval+=m.flipInterval;sum.displayLag+=m.displayLag;
        sum.fenceWait+=m.fenceWait;
        for(unsigned k=0;k<MaxTimedScenes;++k)sum.segmentBusy[k]+=m.segmentBusy[k];
    }
    uint64_t mean(uint64_t total) const noexcept {return frames?total/frames:0;}
    uint64_t flip_mean() const noexcept {return flips?sum.flipInterval/flips:0;}
    // Scenes to report: 0 when the window mixed split and unsplit frames (too few draws).
    unsigned report_scenes() const noexcept {return mixedScenes?0:scenes;}
    // One text receipt per scene and window, per-frame means:
    // "k=<scene>;n=<scenes>;f=<frames>;busy=<us>;draws=;tris=;blend=;discard=;nodepth=;fsb=<fragment bytes per draw>"
    int segment_text(unsigned k,char* out,size_t size) const noexcept {
        if(!size || k>=MaxTimedScenes)return 0;
        const auto& s=segments[k];
        using ull=unsigned long long;
        return std::snprintf(out,size,"k=%u;n=%u;f=%llu;busy=%llu;draws=%llu;tris=%llu;blend=%llu;discard=%llu;nodepth=%llu;fsb=%llu",
            k,report_scenes(),ull(frames),ull(mean(sum.segmentBusy[k])),ull(mean(s.draws)),ull(mean(s.triangles)),
            ull(mean(s.blended)),ull(mean(s.discard)),ull(mean(s.noDepth)),ull(s.draws?s.fragmentBytes/s.draws:0));
    }
};
}
