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
