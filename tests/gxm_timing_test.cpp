// GPU timing diagnostic: per-frame metrics from CPU-observed submit, notification, present and
// flip times (gfx_gxm/gxm_timing.hpp), and the receipt window.
#include "gfx_gxm/gxm_timing.hpp"
#include <cassert>
#include <cstdio>
#include <string>

int main() {
    using namespace twv::gxm;
    {
        // GPU-bound: submitted at 1000 while the previous frame finished only at 4000; done at 15000.
        FrameTimes f;f.submit[0]=1000;f.fragmentDone[0]=15000;f.vertexDone=2500;
        f.presentStart=1100;f.presentEnd=9000;f.flipStart=15100;f.flipEnd=20000;f.fenceWait=300;
        assert(f.complete());
        const auto m=frame_metrics(f,4000,7000,3300);
        assert(m.gpuBusy==11000 && m.segmentBusy[0]==11000 && m.gpuStarved==0);
        assert(m.gpuLatency==14000 && m.vertexTail==1500);
        // AddEntry waited 7900 us: 5900 until the frame two back left the GPU, 2000 for the display.
        assert(m.presentWait==7900 && m.presentGpu==5900 && m.presentDisplay==2000);
        assert(m.flipInterval==16700 && m.displayLag==100 && m.fenceWait==300);
    }
    {
        // CPU-bound: the GPU finished the previous frame at 500 and idled until the submit at 3000.
        FrameTimes f;f.submit[0]=3000;f.fragmentDone[0]=8000;f.presentStart=3100;f.presentEnd=3150;f.flipEnd=9000;
        const auto m=frame_metrics(f,500,0,0);
        assert(m.gpuBusy==5000 && m.gpuStarved==2500 && m.presentGpu==0 && m.presentDisplay==50);
        assert(m.flipInterval==0 && m.displayLag==0 && m.vertexTail==0);
        // The frame two back was done before AddEntry: none of the wait is the GPU's.
        assert(frame_metrics(f,500,3000,0).presentGpu==0);
    }
    {
        // Split frame: three scenes; the second waits for the first, the third for its submit.
        FrameTimes f;f.scenes=3;
        f.submit[0]=1000;f.fragmentDone[0]=3000;
        f.submit[1]=2000;f.fragmentDone[1]=6000;
        f.submit[2]=7000;f.fragmentDone[2]=9500;
        f.presentStart=7100;f.presentEnd=7200;f.flipEnd=10000;
        const auto m=frame_metrics(f,0,0,0);
        assert(m.segmentBusy[0]==2000 && m.segmentBusy[1]==3000 && m.segmentBusy[2]==2500);
        assert(m.gpuBusy==7500 && m.gpuStarved==1000 && m.gpuLatency==2500);
        TimingWindow w;w.add(m,3);w.add(frame_metrics(f,0,0,9000),3);
        assert(w.frames==2 && w.scenes==3 && !w.mixedScenes && w.mean(w.sum.gpuBusy)==7500);
        assert(w.mean(w.sum.segmentBusy[1])==3000 && w.flips==1 && w.flip_mean()==1000 && w.maxGpuBusy==7500);
        w.add(FrameMetrics{},1);
        assert(w.mixedScenes && w.frames==3);
    }
    {
        // Split points from the previous frame's draws; too few draws: one scene.
        uint64_t starts[MaxTimedScenes]{};
        assert(split_points(1000,4,starts)==4 && starts[1]==250 && starts[2]==500 && starts[3]==750);
        assert(split_points(10,3,starts)==3 && starts[1]==4 && starts[2]==7);
        assert(split_points(2,3,starts)==1 && split_points(100,1,starts)==1 && split_points(100,MaxTimedScenes+1,starts)==1);
        assert(triangles(0,9)==3 && triangles(1,5)==3 && triangles(2,2)==0 && triangles(3,8)==4);
        // Segment receipt: per-frame means per scene.
        FrameTimes f;f.scenes=2;f.submit[0]=100;f.fragmentDone[0]=1100;f.submit[1]=200;f.fragmentDone[1]=4100;f.flipEnd=5000;
        SegmentStats s[2];s[0].draws=10;s[0].triangles=500;s[0].fragmentBytes=4000;s[1].draws=4;s[1].blended=4;s[1].noDepth=2;
        s[1].discard=1;s[1].triangles=8;s[1].fragmentBytes=1200;
        TimingWindow w;w.add(frame_metrics(f,0,0,0),2,s);w.add(frame_metrics(f,0,0,0),2,s);
        char text[256];
        assert(w.report_scenes()==2 && w.segment_text(0,text,sizeof text)>0);
        assert(std::string(text)=="k=0;n=2;f=2;busy=1000;draws=10;tris=500;blend=0;discard=0;nodepth=0;fsb=400");
        w.segment_text(1,text,sizeof text);
        assert(std::string(text)=="k=1;n=2;f=2;busy=3000;draws=4;tris=8;blend=4;discard=1;nodepth=2;fsb=300");
        assert(w.segment_text(MaxTimedScenes,text,sizeof text)==0);
        w.add(FrameMetrics{},1,s);
        assert(w.report_scenes()==0); // a window that mixed split and unsplit frames reports no scenes
    }
    {
        // Incomplete records are never summed.
        FrameTimes f;f.submit[0]=1;f.fragmentDone[0]=2;
        assert(!f.complete());f.flipEnd=3;assert(f.complete());
        f.scenes=2;assert(!f.complete());f.scenes=0;assert(!f.complete());f.scenes=MaxTimedScenes+1;assert(!f.complete());
    }
    std::printf("gxm_timing_test: GPU/CPU-bound, present split, split scenes, window and incomplete records passed\n");
}
