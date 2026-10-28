// G0 start-up options (--vita-gpu, --vita-gpu-digest, --vita-capture, --vita-capture-match)
// and the capture schedule (frame index, digest match, limits).
#include "runtime/gpu_options.hpp"
#include "gfx_vgl/frame_capture.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using twv::vita::GpuOptions;
    {
        GpuOptions o;
        assert(o.backend==0 && o.digest==0 && !o.captureStep && o.consistent());
        assert(o.consume("--vita-clocks=system")==0 && o.consume("--vita-gpux=vgl")==0 && o.consume("--vita-gpu")==0);
        assert(o.consume("--vita-gpu=gxm")==1 && o.backend==1);
        assert(o.consume("--vita-gpu=vgl")==-1); // once only
        assert(o.consume("--vita-gpu-digest=on")==1 && o.digest==1);
        assert(o.consume("--vita-capture=300-900/60")==1 && o.captureFirst==300 && o.captureLast==900 && o.captureStep==60);
        assert(o.consume("--vita-capture-match=on")==1 && o.captureMatch==1 && o.consistent());
    }
    {
        GpuOptions o;
        assert(o.consume("--vita-gpu=GXM")==-1 && o.consume("--vita-gpu=")==-1);
        assert(o.consume("--vita-gpu-digest=draws:10-20")==1 && o.digest==2 && o.digestFirst==10 && o.digestLast==20);
        for(const char* t:{"--vita-gpu-digest=draws:20-10","--vita-gpu-digest=draws:0-5","--vita-gpu-digest=draws:1-5000",
                           "--vita-gpu-digest=draws:5","--vita-gpu-digest=yes"})
            assert(GpuOptions{}.consume(t)==-1);
    }
    {
        for(const char* t:{"--vita-capture=300-900","--vita-capture=300-900/0","--vita-capture=1-1000/1",
                           "--vita-capture=900-300/10","--vita-capture=a-b/c","--vita-capture=1-2/3x"})
            assert(GpuOptions{}.consume(t)==-1);
        GpuOptions o;
        assert(o.consume("--vita-capture=1-48/1")==1);
        GpuOptions m;
        assert(m.consume("--vita-capture-match=on")==1 && !m.consistent()); // matching needs the digest
    }
    {
        // G2 features: GXM only, each once.
        GpuOptions o;
        assert(o.consume("--vita-gxm-primitives=on")==1 && o.consume("--vita-gxm-primitives=off")==-1);
        assert(o.consume("--vita-gxm-compact=colors")==1 && o.consume("--vita-gxm-resident=on")==1);
        assert(o.features==(GpuOptions::FeaturePrimitives|GpuOptions::FeatureCompact|GpuOptions::FeatureCompactColors|
                            GpuOptions::FeatureResident));
        assert(o.any() && !o.consistent()); // still vgl
        assert(o.consume("--vita-gpu=gxm")==1 && o.consistent());
        GpuOptions c;
        assert(c.consume("--vita-gxm-compact=on")==1 && c.features==GpuOptions::FeatureCompact);
        assert(GpuOptions{}.consume("--vita-gxm-compact=all")==-1 && GpuOptions{}.consume("--vita-gxm-resident=1")==-1);
        assert(!GpuOptions{}.any());
        // Play profile: a backend or drawing feature is a choice; digest, captures and GPU diagnostics are not.
        assert(c.any_choice() && !GpuOptions{}.any_choice());
        GpuOptions d;
        assert(d.consume("--vita-gpu-digest=on")==1 && d.consume("--vita-capture=1-2/1")==1 &&
               d.consume("--vita-gxm-gpu-timing=on")==1 && d.consume("--vita-gxm-scale=75")==1 && !d.any_choice());
        GpuOptions b;assert(b.consume("--vita-gpu=vgl")==1 && b.any_choice());
        // Uniform reuse: GXM only, once, an addition the play profile accepts (not a choice).
        GpuOptions u;
        assert(u.consume("--vita-gxm-uniform-reuse=on")==1 && u.features==GpuOptions::FeatureUniformReuse);
        assert(u.any() && !u.any_choice() && !u.consistent() && u.consume("--vita-gxm-uniform-reuse=off")==-1);
        assert(GpuOptions{}.consume("--vita-gxm-uniform-reuse=yes")==-1);
        // Pipeline memo: the same kind of addition, alongside the uniform reuse.
        assert(u.consume("--vita-gxm-pipeline-memo=on")==1 &&
               u.features==(GpuOptions::FeatureUniformReuse|GpuOptions::FeaturePipelineMemo));
        assert(!u.any_choice() && u.consume("--vita-gxm-pipeline-memo=on")==-1 && u.consume("--vita-gpu=gxm")==1 &&
               u.consistent());
        GpuOptions pm;assert(pm.consume("--vita-gxm-pipeline-memo=off")==1 && pm.features==0 && pm.any());
        assert(GpuOptions{}.consume("--vita-gxm-pipeline-memo")==0 && GpuOptions{}.consume("--vita-gxm-pipeline-memo=1")==-1);
    }
    {
        // GPU diagnostics: GXM only, once.
        GpuOptions o;
        assert(o.consume("--vita-gxm-gpu-timing=on")==1 && o.features==GpuOptions::FeatureTiming && o.any());
        assert(!o.consistent() && o.consume("--vita-gxm-gpu-timing=off")==-1);
        assert(o.consume("--vita-gpu=gxm")==1 && o.consistent());
        GpuOptions off;assert(off.consume("--vita-gxm-gpu-timing=off")==1 && off.features==0 && off.any() && off.consistent());
        assert(GpuOptions{}.consume("--vita-gxm-gpu-timing=yes")==-1 && GpuOptions{}.consume("--vita-gxm-gpu-timing")==0);
        // The split implies the timing; 2..8 scenes, once.
        GpuOptions s;
        assert(s.consume("--vita-gxm-gpu-split=4")==1 && s.splitScenes==4 &&
               s.features==(GpuOptions::FeatureSplit|GpuOptions::FeatureTiming) && !s.consistent());
        assert(s.consume("--vita-gxm-gpu-split=4")==-1 && s.consume("--vita-gpu=gxm")==1 && s.consistent());
        for(const char* t:{"--vita-gxm-gpu-split=1","--vita-gxm-gpu-split=9","--vita-gxm-gpu-split=4x",
                           "--vita-gxm-gpu-split=","--vita-gxm-gpu-split=-2"})
            assert(GpuOptions{}.consume(t)==-1);
        assert(GpuOptions{}.consume("--vita-gxm-gpu-split=8")==1);
        // Render scale: one of four sizes, once, GXM only.
        GpuOptions r;
        assert(r.consume("--vita-gxm-scale=75")==1 && r.features==GpuOptions::FeatureScale75 && !r.consistent());
        assert(r.consume("--vita-gxm-scale=50")==-1 && r.consume("--vita-gpu=gxm")==1 && r.consistent());
        GpuOptions full;assert(full.consume("--vita-gxm-scale=100")==1 && full.features==0 && full.any());
        assert(GpuOptions{}.consume("--vita-gxm-scale=67")==1 && GpuOptions{}.consume("--vita-gxm-scale=60")==-1);
    }
    {
        twv::gpu::CaptureSchedule s;s.first=10;s.last=30;s.step=10;
        assert(s.active() && !s.wants(9,0) && s.wants(10,0) && !s.wants(15,0) && s.wants(20,0) && s.wants(30,0) && !s.wants(40,0));
        assert(s.load_match("# comment\r\n00000000000000aa\r\n\n00000000000000BB\n")==false);
        assert(s.load_match("# comment\r\n00000000000000aa\r\n\n00000000000000bb\n") && s.match.size()==2);
        assert(!s.wants(41,0x99) && s.wants(42,0xaa) && !s.wants(43,0xaa)); // once per listed digest
        assert(s.wants(44,0xbb) && s.taken==5);
        assert(!s.load_match("0123") && !s.load_match("00000000000000aaa\n") && s.match.empty() && s.matched.empty());
        twv::gpu::CaptureSchedule many;many.first=1;many.last=1000;many.step=1;
        unsigned taken=0;for(uint32_t f=1;f<=1000;++f)taken+=many.wants(f,0);
        assert(taken==twv::gpu::CaptureSchedule::CaptureLimit);
        char name[96];twv::gpu::capture_name(name,sizeof name,"root",42,0xabcULL);
        assert(std::strcmp(name,"root/f000042-0000000000000abc.png")==0);
    }
    std::printf("gpu_options_test: backend/digest/capture parsing, repeats, ranges and capture schedule passed\n");
}
