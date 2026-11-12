// SDK domain: the exported twv_vgl_* interface (gpu_api.h). Each call goes to the
// backend chosen at start-up; with --vita-gpu-digest the same calls also feed the
// per-frame draw digest, so two backends or two runs can be compared draw for draw.
// Calls are serialised by the FIFO drain, as before (one backend owner at a time). With
// --vita-gxm-submit-thread (submit_thread.hpp) draws and their sampler units go through a queue to
// a third thread; every other call below first waits until that queue is empty. With
// --vita-gxm-draw-merge (draw_merge.hpp) a quad or triangle-list draw is held back until the next
// call, which either joins it or sends it on first.
#include "gpu_backend.h"
#include "gpu_common.hpp"
#include "draw_digest.hpp"
#include "draw_merge.hpp"
#include "frame_capture.hpp"
#include "submit_thread.hpp"
#include "runtime/telemetry.hpp"
#include "ui/startup_screen.hpp"
#include <psp2/kernel/processmgr.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
const TwvGpuBackend* backend=nullptr; // chosen once, before init
bool selected=false,initialised=false;
uint32_t requestedFeatures=0;          // G2 TwvGpuFeature* bits, a subset of backend->features
uint32_t splitScenes=0;                // --vita-gxm-gpu-split (with TwvGpuFeatureSplit)
int digestMode=0; // 0 off, 1 per-frame, 2 per-frame + per-draw hashes in a frame window
uint32_t digestFirst=0,digestLast=0,frameIndex=0; // frameIndex: twv_vgl_end calls so far
std::unique_ptr<twv::gpu::DrawDigest> digest;
twv::gpu::CaptureSchedule capture;
FILE* drawLog=nullptr;
uint64_t digestDraws=0;
constexpr const char* CaptureRoot="ux0:data/twilight-vita/captures";
constexpr const char* MatchFile="ux0:data/twilight-vita/capture_match.txt";
constexpr const char* DrawLogFile="ux0:data/twilight-vita/digest_draws.txt";

const TwvGpuBackend& gpu() {
    if(!backend)backend=twv_gpu_backend_vgl();
    return *backend;
}
// --vita-gxm-draw-merge: where a draw goes once the hold lets it through. With the submit thread
// a held draw's state lives in the queue slot it will travel in (one copy, as without the merge).
struct MergeSink {
    twv::gpu::SubmitCommand* reserved=nullptr;
    TwvVitaDrawState own;
    TwvVitaDrawState* reserve() {
        if(!twv::gpu::submit::active())return &own;
        reserved=twv::gpu::submit::begin_draw();
        return &reserved->state;
    }
    void submit(const TwvVitaDrawState* state,const void* vertices,uint32_t count,const uint16_t* indices,
                uint32_t indexCount,const twv::gpu::DrawUnits& units) {
        if(twv::gpu::submit::active()) {
            twv::gpu::SubmitCommand* c=reserved;reserved=nullptr;
            if(!c) {
                c=twv::gpu::submit::begin_draw();
                if(state)c->state=*state;
            }
            c->vertices=vertices;c->indices=indices;c->count=count;c->indexCount=indexCount;
            c->hasState=state!=nullptr;c->units=units;
            twv::gpu::submit::end_draw();
            return;
        }
        for(unsigned i=0;units.mask>>i;++i)
            if(units.mask&(1u<<i))
                gpu().texture_sampler_unit(i,units.params[i][0],units.params[i][1],units.params[i][2],units.params[i][3]);
        gpu().draw(state,vertices,count,indices,indexCount);
    }
};
MergeSink mergeSink;
std::unique_ptr<twv::gpu::DrawMerger<MergeSink>> merger; // only with the switch
// Before every backend call that is not a draw or a sampler-unit request: the held draw goes on,
// then the submit thread finishes what it has.
void settle() {
    if(merger)merger->flush();
    twv::gpu::submit::drain();
}
// With --vita-gxm-draw-split=on (diagnostic): which draws repeat the draw before them. A draw is
// "same" when its whole state is byte-identical to the previous draw's (matrices included). Counted
// by primitive (triangles, strip, fan, quads), by whether it carries an index list, and by size
// (at most 8 vertices, at most 32, more); and how many same draws start where the previous draw's
// vertices end. Receipts gfx_same_draws_* every 60 frames (running totals).
struct SameCensus {
    TwvVitaDrawState previous;
    const unsigned char* previousEnd=nullptr;
    bool have=false;
    uint64_t draws[4][2]={},same[4][2]={},sameSize[3]={},sameAdjacent=0,sameIndexedVertices=0;
} sameCensus;
void same_census(const TwvVitaDrawState* state,const void* vertices,uint32_t count,uint32_t indexCount) {
    if(!state)return;
    const unsigned prim=state->primitive<4?state->primitive:0,indexed=indexCount!=0;
    ++sameCensus.draws[prim][indexed];
    const auto* start=static_cast<const unsigned char*>(vertices);
    if(sameCensus.have && std::memcmp(&sameCensus.previous,state,sizeof *state)==0) {
        ++sameCensus.same[prim][indexed];
        ++sameCensus.sameSize[count<=8?0:count<=32?1:2];
        if(start==sameCensus.previousEnd)++sameCensus.sameAdjacent;
        if(indexed)sameCensus.sameIndexedVertices+=count;
    }
    sameCensus.previous=*state;sameCensus.have=true;
    sameCensus.previousEnd=start?start+size_t(count)*state->vertex_layout.stride:nullptr;
}
void same_census_report() {
    static const char* const prims[4]={"triangles","strip","fan","quads"};
    char name[64];
    for(unsigned p=0;p<4;++p)for(unsigned i=0;i<2;++i) {
        std::snprintf(name,sizeof name,"gfx_same_draws_all_%s_%s",prims[p],i?"indexed":"plain");
        twv::vita::telemetryEvent(name,static_cast<long long>(sameCensus.draws[p][i]));
        std::snprintf(name,sizeof name,"gfx_same_draws_same_%s_%s",prims[p],i?"indexed":"plain");
        twv::vita::telemetryEvent(name,static_cast<long long>(sameCensus.same[p][i]));
    }
    twv::vita::telemetryEvent("gfx_same_draws_same_upto8",static_cast<long long>(sameCensus.sameSize[0]));
    twv::vita::telemetryEvent("gfx_same_draws_same_upto32",static_cast<long long>(sameCensus.sameSize[1]));
    twv::vita::telemetryEvent("gfx_same_draws_same_larger",static_cast<long long>(sameCensus.sameSize[2]));
    twv::vita::telemetryEvent("gfx_same_draws_same_adjacent",static_cast<long long>(sameCensus.sameAdjacent));
}
void merge_report() {
    if(requestedFeatures&TwvGpuFeatureDrawSplit)same_census_report();
    if(!merger)return;
    static const char* const names[twv::gpu::MergeRefusalCount]={"gfx_draw_merge_merged","gfx_draw_merge_refused_shape",
        "gfx_draw_merge_refused_state","gfx_draw_merge_refused_units","gfx_draw_merge_refused_apart",
        "gfx_draw_merge_refused_size"};
    twv::vita::telemetryEvent("gfx_draw_merge_draws",static_cast<long long>(merger->draws));
    for(unsigned i=0;i<twv::gpu::MergeRefusalCount;++i)
        twv::vita::telemetryEvent(names[i],static_cast<long long>(merger->refused[i]));
}
bool logging_draws() {return drawLog && frameIndex+1>=digestFirst && frameIndex+1<=digestLast;}
void load_capture_match() {
    FILE* file=std::fopen(MatchFile,"rb");
    if(!file){twv::vita::telemetryText("capture_match_error","capture_match.txt missing");return;}
    std::vector<char> text(64*1024+1,0);
    const size_t n=std::fread(text.data(),1,text.size()-1,file);
    std::fclose(file);
    if(n==text.size()-1 || !capture.load_match(text.data())) {
        twv::vita::telemetryText("capture_match_error","capture_match.txt invalid");capture.match.clear();
    }
    twv::vita::telemetryEvent("capture_match_digests",static_cast<long long>(capture.match.size()));
}
}

extern "C" int twv_vita_gpu_select(int which,int digestOption,uint32_t first,uint32_t last,
                                   uint32_t captureFirst,uint32_t captureLast,uint32_t captureStep,
                                   int captureMatch,uint32_t features,uint32_t split) {
    if(selected || initialised || which<0 || which>1 || digestOption<0 || digestOption>2)return 0;
#if TWV_VITA_GFX_GXM
    const TwvGpuBackend* chosen=which==0?twv_gpu_backend_vgl():twv_gpu_backend_gxm();
#else
    const TwvGpuBackend* chosen=which==0?twv_gpu_backend_vgl():nullptr;
#endif
    if(!chosen || (features & ~chosen->features))return 0;
    if(((features&TwvGpuFeatureSplit)!=0)!=(split>=2 && split<=8))return 0;
    backend=chosen;selected=true;requestedFeatures=features;splitScenes=split;
    digestMode=digestOption;digestFirst=first;digestLast=last;
    capture.first=captureFirst;capture.last=captureLast;capture.step=captureStep;
    if(captureMatch && digestMode)load_capture_match();
    return 1;
}

extern "C" uint32_t twv_gpu_features(void) {return requestedFeatures;}
extern "C" uint32_t twv_gpu_split_scenes(void) {return splitScenes;}
// patches/aurora/0008: whether quads, strips and fans reach the backend without index lists.
extern "C" int twv_gx_native_primitives(void) noexcept {return (requestedFeatures&TwvGpuFeaturePrimitives)!=0;}
extern "C" int twv_gpu_resident_available(void) {return gpu().resident_available();}
extern "C" void* twv_gpu_resident_alloc(size_t bytes) {return gpu().resident_alloc(bytes);}
extern "C" void twv_gpu_resident_release(void* ptr) {gpu().resident_release(ptr);}

extern "C" int twv_vgl_init(void) {
    initialised=true;
    twv::gpu::common_init();
    twv::vita::telemetryText("gpu_backend",gpu().name);
    twv::vita::telemetryEvent("gpu_features",requestedFeatures);
    twv::vita::telemetryEvent("gpu_digest_mode",digestMode);
    if(digestMode)digest=std::make_unique<twv::gpu::DrawDigest>();
    if(digestMode==2) {
        drawLog=std::fopen(DrawLogFile,"wb");
        if(drawLog)std::setvbuf(drawLog,nullptr,_IOFBF,256*1024);
        else twv::vita::telemetryText("gpu_digest_error","digest_draws.txt not writable");
    }
    if(capture.active()) {
        mkdir(CaptureRoot,0777);
        twv::vita::telemetryEvent("capture_first",capture.first);
        twv::vita::telemetryEvent("capture_last",capture.last);
        twv::vita::telemetryEvent("capture_step",capture.step);
    }
    if(gpu().init()) {
        if(requestedFeatures&TwvGpuFeatureSubmitThread)
            twv::gpu::submit::start(gpu(),(requestedFeatures&TwvGpuFeatureSubmitVerify)!=0);
        if(requestedFeatures&TwvGpuFeatureDrawMerge) {
            merger=std::make_unique<twv::gpu::DrawMerger<MergeSink>>(mergeSink);
            merger->set_verify((requestedFeatures&TwvGpuFeatureDrawMergeVerify)!=0);
            twv::vita::telemetryEvent("gfx_draw_merge",(requestedFeatures&TwvGpuFeatureDrawMergeVerify)?2:1);
        }
        return 1;
    }
    // Fail closed: the backend's own receipts name the failing call and its error code (gxm_error,
    // gfx_init_failure); this one names the backend, and the run ends here with exit code 3 instead
    // of an abort dialog or a black screen: the start-up screen says what the backend noted (missing
    // shader compiler, memory, the failing call) and waits for a button.
    char text[96];
    std::snprintf(text,sizeof text,"%s backend initialization failed; exiting",gpu().name);
    twv::vita::telemetryText("gfx_init_failure",text);
    twv::vita::startupGraphicsFailureExit(3);
}
extern "C" void twv_vgl_shutdown(void) {
    if(merger)merger->flush();
    twv::gpu::submit::stop();
    gpu().shutdown();
    if(drawLog){std::fclose(drawLog);drawLog=nullptr;}
}
extern "C" void twv_vgl_begin(void) {settle();gpu().begin();}
extern "C" void twv_vgl_end(void) {
    const uint32_t index=++frameIndex;
    uint64_t frameDigest=0;
    if(digest) {
        const auto frame=digest->end_frame();
        frameDigest=frame.digest;digestDraws+=frame.draws;
        char text[160];
        std::snprintf(text,sizeof text,"f=%lu d=%016llx n=%lu c=%lu v=%lu i=%lu t=%lu p=%lu",
            static_cast<unsigned long>(index),static_cast<unsigned long long>(frame.digest),
            static_cast<unsigned long>(frame.draws),static_cast<unsigned long>(frame.clears),
            static_cast<unsigned long>(frame.vertices),static_cast<unsigned long>(frame.indices),
            static_cast<unsigned long>(frame.textureWrites),static_cast<unsigned long>(frame.programs));
        twv::vita::telemetryText("gfx_digest",text);
        if(drawLog && index>=digestFirst && index<=digestLast)
            std::fprintf(drawLog,"F %lu %016llx\n",static_cast<unsigned long>(index),
                static_cast<unsigned long long>(frame.digest));
    }
    char path[160];const char* capturePath=nullptr;
    if(capture.active() && capture.wants(index,frameDigest)) {
        twv::gpu::capture_name(path,sizeof path,CaptureRoot,index,frameDigest);
        capturePath=path;
    }
    settle();
    if(!gpu().end(capturePath))return;
    twv::gpu::common_frame(gpu());
    if(twv::gpu::frames_presented()%60==0){twv::gpu::submit::report();merge_report();}
    if(digest && twv::gpu::frames_presented()%60==0) {
        twv::vita::telemetryEvent("gpu_digest_draws",static_cast<long long>(digestDraws));
        twv::vita::telemetryEvent("gpu_digest_live_textures",static_cast<long long>(digest->live_textures()));
        twv::vita::telemetryEvent("gpu_digest_live_programs",static_cast<long long>(digest->live_programs()));
        if(drawLog)std::fflush(drawLog);
    }
}
extern "C" void* twv_vgl_alloc(size_t size) {return gpu().alloc(size);}
extern "C" uint32_t twv_vgl_vertex_is_vram(const void* address) {return gpu().vertex_is_vram(address);}
extern "C" uint32_t twv_vgl_vertex_write_class(const void* address,size_t bytes) {
    return gpu().vertex_write_class(address,bytes);
}
extern "C" void twv_vgl_vertex_write_class_report(void) {gpu().vertex_write_class_report();}
extern "C" uint32_t twv_vgl_program(const char* vertex,const char* fragment) {
    settle();
    const uint32_t id=gpu().program(vertex,fragment);
    if(digest)digest->program_created(id,vertex,fragment);
    return id;
}
extern "C" void twv_vgl_program_delete(uint32_t program) {settle();gpu().program_delete(program);}
extern "C" void twv_vgl_draw(const TwvVitaDrawState* state,const void* vertices,uint32_t count,
                             const uint16_t* indices,uint32_t indexCount) {
    if(digest && state) {
        const uint64_t value=digest->draw(*state,vertices,count,indices,indexCount);
        if(logging_draws())std::fprintf(drawLog,"D %016llx %lu %lu\n",static_cast<unsigned long long>(value),
            static_cast<unsigned long>(count),static_cast<unsigned long>(indexCount));
    }
    if(requestedFeatures&TwvGpuFeatureDrawSplit)same_census(state,vertices,count,indexCount);
    if(merger)merger->draw(state,vertices,count,indices,indexCount);
    else if(twv::gpu::submit::active())twv::gpu::submit::draw(state,vertices,count,indices,indexCount);
    else gpu().draw(state,vertices,count,indices,indexCount);
}
extern "C" uint32_t twv_vgl_texture(uint32_t width,uint32_t height,uint32_t levels) {
    settle();
    const uint32_t id=gpu().texture(width,height,levels);
    if(digest)digest->texture_created(id,width,height,levels);
    return id;
}
extern "C" uint32_t twv_vgl_texture_static_rgba(uint32_t width,uint32_t height,uint32_t levels,
                                                const void* bytes,size_t size) {
    settle();
    const uint32_t id=gpu().texture_static_rgba(width,height,levels,bytes,size);
    if(digest)digest->texture_static(id,width,height,levels,bytes,size);
    return id;
}
extern "C" void twv_vgl_texture_write(uint32_t texture,uint32_t level,uint32_t width,uint32_t height,const void* rgba) {
    if(digest)digest->texture_written(texture,level,width,height,rgba);
    settle();
    gpu().texture_write(texture,level,width,height,rgba);
}
extern "C" void twv_vgl_texture_delete(uint32_t texture) {
    if(digest)digest->texture_deleted(texture);
    settle();
    gpu().texture_delete(texture);
}
extern "C" void twv_vgl_texture_sampler(uint32_t texture,int min,int mag,int s,int t) {
    if(digest)digest->texture_sampler(texture,min,mag,s,t);
    settle();
    gpu().texture_sampler(texture,min,mag,s,t);
}
extern "C" int twv_vgl_texture_sampler_unit(uint32_t unit,int min,int mag,int s,int t) {
    // Queued in order with the draws. The GXM backend accepts every unit below 8 once it is ready
    // (it is: the queue only exists after init); any other unit goes straight to it, after a drain.
    // With the merge the request stays here until its draw goes on (draw_merge.hpp).
    int accepted=1;
    if(merger && unit<8)merger->unit(unit,min,mag,s,t);
    else if(twv::gpu::submit::active() && unit<8)twv::gpu::submit::sampler_unit(unit,min,mag,s,t);
    else {settle();accepted=gpu().texture_sampler_unit(unit,min,mag,s,t);}
    if(digest)digest->sampler_unit(unit,min,mag,s,t,accepted);
    return accepted;
}
extern "C" void twv_vgl_clear(float r,float g,float b,float a,float depth,unsigned mask,int x,int y,int w,int h) {
    if(digest)digest->clear(r,g,b,a,depth,mask,x,y,w,h);
    settle();
    gpu().clear(r,g,b,a,depth,mask,x,y,w,h);
}
