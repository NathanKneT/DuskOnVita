// SDK domain: the direct GXM backend (G1, --vita-gpu=gxm). It implements the same
// twv_vgl_* table as the vitaGL backend and draws what vitaGL drew for the same calls:
// - sceGxmInitialize with flags 0; three display buffers, two pending flips;
// - CDRAM texture pool with a first-fit sub-allocator; LPDDR frame ring in three slots;
// - frame slots fenced by EndScene fragment notifications; no sceGxmFinish in the loop;
// - Cg compiled with the user's libshacccg.suprx through SceShaccCg (vitaGL's settings),
//   cached as GXP on the memory card per source/options/compiler, validated with
//   sceGxmProgramCheck, one compile at a time;
// - patcher programs per vertex layout and per blend state; default uniform buffers
//   reserved from the context ring, filled from per-program uniform copies (GL semantics);
// - RGBA8 linear textures with per-draw sampler words; clears drawn as rectangles.
// One owner at a time: calls are serialised by the FIFO drain, as for vitaGL.
#include "gfx_vgl/gpu_backend.h"
#include "gfx_vgl/frame_capture.hpp"
#include "gfx_vgl/sampler_state.hpp"
#include "gfx_vgl/shader_preparation.hpp"
#include "gfx_vgl/staging_policy.hpp"
#include "gfx_vgl/texture_upload.hpp"
#include "gfx_vgl/texture_upload_api.h"
#include "gxm_memory.hpp"
#include "gxm_shader_cache.hpp"
#include "gxm_state.hpp"
#include "gxm_timing.hpp"
#include "runtime/telemetry.hpp"
#include "ui/startup_screen.hpp"
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/shacccg.h>
#include <shacccg_ext.h>
#include <sys/stat.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace twv::gxm;
// gxm_state.hpp mirrors these SDK values for host tests.
static_assert(SCE_GXM_BLEND_FUNC_NONE==BlendFuncNone && SCE_GXM_BLEND_FUNC_ADD==BlendFuncAdd &&
              SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT==BlendFuncReverseSubtract);
static_assert(SCE_GXM_BLEND_FACTOR_SRC_COLOR==FactorSrcColor && SCE_GXM_BLEND_FACTOR_DST_COLOR==FactorDstColor &&
              SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA==FactorOneMinusDstAlpha);
static_assert(SCE_GXM_COLOR_MASK_A==MaskA && SCE_GXM_COLOR_MASK_R==MaskR && SCE_GXM_COLOR_MASK_B==MaskB);
static_assert(SCE_GXM_CULL_CW==CullCw && SCE_GXM_CULL_CCW==CullCcw);
static_assert(SCE_GXM_DEPTH_FUNC_LESS==depth_func(1) && SCE_GXM_DEPTH_FUNC_ALWAYS==depth_func(7));
static_assert(SCE_GXM_TEXTURE_FILTER_LINEAR==FilterLinear && SCE_GXM_TEXTURE_MIP_FILTER_ENABLED==MipFilterEnabled);
static_assert(SCE_GXM_TEXTURE_ADDR_MIRROR==AddrMirror && SCE_GXM_TEXTURE_ADDR_CLAMP==AddrClamp);

constexpr unsigned DisplayBuffers=3;
constexpr uint32_t DisplayStride=960; // ALIGN(960,64)
constexpr uint32_t DisplayBytes=DisplayStride*DisplayHeight*4;
// Render size: the display's, or (--vita-gxm-scale, diagnostic) 720x408, 640x368 or 480x272,
// which the display hardware scales up. Draw coordinates stay in 960x544 and are scaled when set.
uint32_t renderWidth=DisplayWidth,renderHeight=DisplayHeight,renderStride=DisplayStride;
float renderScaleX=1.f,renderScaleY=1.f;
// Sized from the G1 peaks in house, Ordon and the Faron fight (Vita3K): frame ring 4.0 MiB per
// frame, texture pool 11.9 MiB; about twice the peak each. Resident geometry is new in G2.
constexpr uint32_t RingSlotBytes=8u<<20;
constexpr uint32_t TexturePoolBytes=32u<<20,ResidentPoolBytes=24u<<20;
constexpr uint32_t TextureAlignment=256,ResidentAlignment=16;
constexpr uint32_t QuadTableQuads=16384; // 65536 vertices
constexpr const char* GxpRoot="ux0:data/twilight-vita/gxp_cache";

struct Block {SceUID uid=-1;void* base=nullptr;uint32_t size=0;};
struct DisplayData {void* address;uint64_t frame;}; // frame: for the GPU timing diagnostic

// --- state ---
bool ready=false,inScene=false;
SceGxmContext* ctx=nullptr;
SceGxmRenderTarget* target=nullptr;
SceGxmShaderPatcher* patcher=nullptr;
void* contextHost=nullptr;
Block vdmRing,vertexRing,fragmentRing,fragmentUsseRing,patcherBuffer,patcherVertexUsse,patcherFragmentUsse;
std::array<Block,DisplayBuffers> displayMemory;
std::array<SceGxmColorSurface,DisplayBuffers> displaySurface{};
std::array<SceGxmSyncObject*,DisplayBuffers> displaySync{};
Block depthMemory;
SceGxmDepthStencilSurface depthSurface{};
Block texturePool,ringMemory,staticMemory,residentMemory;
RangeAllocator textureAllocator,residentAllocator;
FrameRing ring;
uint16_t* linearIndices=nullptr;uint16_t* quadIndices=nullptr;float* zeroAttribute=nullptr;
uint16_t* quadTable=nullptr; // GX quads as triangles (0,1,2),(2,3,0): Aurora's expansion
std::unordered_map<uint32_t,uint32_t> residentSizes; // offset -> bytes
volatile unsigned int* notifications=nullptr;
uint64_t frameSerial=0,completedFrame=0;
unsigned frontBuffer=0,backBuffer=1;

// Counters (receipts every 60 frames).
uint32_t errors=0,draws=0,frames=0;
uint64_t frameWaits=0,frameWaitUs=0,captureWaits=0;
uint64_t stageCompiles=0,stageDiskHits=0,stageDiskRejects=0,stageMemoryHits=0,stageFailures=0;
uint64_t programCacheHits=0,uniquePrograms=0,vertexVariants=0,fragmentVariants=0,genericAttributeVariants=0;
uint64_t uniformReserveFailures=0,missingTextureBinds=0,textureAllocFailures=0,textureCopies=0,textureCopyBytes=0;
uint64_t ringFailures=0,drawsSkipped=0,clears=0;
uint64_t residentAllocs=0,residentFailures=0,residentReleases=0,primitiveDraws[4]{},packedVariants=0;

bool initializing=false; // gxm_init: a failure is noted for the start-up error screen
bool ok(int result,const char* call) {
    if(result>=0)return true;
    ++errors;
    char text[96];std::snprintf(text,sizeof text,"%s 0x%08x",call,unsigned(result));
    if(errors<=16)twv::vita::telemetryText("gxm_error",text);
    if(initializing)twv::vita::startupNoteProblem(twv::vita::StartupProblem::Graphics,text,0,0);
    return false;
}
void* gpu_block(Block& b,const char* name,SceKernelMemBlockType type,uint32_t size,int attribs) {
    const uint32_t align=type==SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW?256u*1024u:4096u;
    size=(size+align-1)&~(align-1);
    b.uid=sceKernelAllocMemBlock(name,type,size,nullptr);
    if(b.uid<0 && initializing) { // the start-up screen names the memory that ran short
        const bool cdram=type==SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
        SceKernelFreeMemorySizeInfo info{};info.size=sizeof info;
        const bool known=sceKernelGetFreeMemorySize(&info)>=0;
        twv::vita::startupNoteProblem(twv::vita::StartupProblem::Memory,cdram?"video memory (CDRAM)":"main memory",
            known?(cdram?info.size_cdram:info.size_user):0,known?size:0);
    }
    if(!ok(b.uid,name))return nullptr;
    if(!ok(sceKernelGetMemBlockBase(b.uid,&b.base),name)){sceKernelFreeMemBlock(b.uid);b.uid=-1;return nullptr;}
    if(attribs && !ok(sceGxmMapMemory(b.base,size,SceGxmMemoryAttribFlags(attribs)),name)) {
        sceKernelFreeMemBlock(b.uid);b.uid=-1;b.base=nullptr;return nullptr;
    }
    b.size=size;return b.base;
}
void* usse_block(Block& b,const char* name,uint32_t size,bool vertex,unsigned int* offset) {
    if(!gpu_block(b,name,SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,size,0))return nullptr;
    const int result=vertex?sceGxmMapVertexUsseMemory(b.base,b.size,offset):sceGxmMapFragmentUsseMemory(b.base,b.size,offset);
    if(!ok(result,name)){sceKernelFreeMemBlock(b.uid);b.uid=-1;b.base=nullptr;return nullptr;}
    return b.base;
}
// --- GPU timing diagnostic (--vita-gxm-gpu-timing, gxm_timing.hpp) ---
// A monitor thread (core 2, top user priority, asleep on a semaphore or a notification) stamps
// the frame's vertex notification and each scene's fragment notification as the GPU writes
// them; the display callback stamps the flip. The main thread sums each frame into the
// receipt window once all its stamps are in, in frame order.
constexpr unsigned TimingRing=16,TimingQueue=64;
constexpr unsigned VertexNotificationBase=8; // slots 8..10; 0..2 are the frame fences
struct TimedFrame {
    std::atomic<uint64_t> frame{0};
    std::array<std::atomic<uint64_t>,MaxTimedScenes> fragmentDone{};
    std::atomic<uint64_t> vertexDone{0},flipStart{0},flipEnd{0};
    FrameTimes main; // submit, present and fence fields: main thread only
    std::array<SegmentStats,MaxTimedScenes> segments{}; // per scene (--vita-gxm-gpu-split): main thread
};
struct TimedWait {uint64_t frame;uint32_t scene;bool vertex;volatile unsigned int* address;unsigned int value;};
bool timingOn=false;
std::array<TimedFrame,TimingRing> timedFrames;
std::array<TimedWait,TimingQueue> timedWaits{};
std::atomic<uint32_t> timedHead{0},timedTail{0};
std::atomic<bool> timingStop{false};
SceUID timingSema=-1,timingThread=-1;
uint64_t timedFinal=0,timingDropped=0,timingQueueFull=0;
TimingWindow timingWindow;
// Scene split (--vita-gxm-gpu-split): the frame's draws in up to splitScenes scenes, cut at equal
// shares of the previous frame's draws. Depth (with stencil and mask bit) is stored at the end of
// every scene but the last and reloaded at the start of every scene but the first, so the picture
// is the frame's; colour is always loaded and stored. Each scene ends with its own notification.
constexpr unsigned SceneNotificationBase=16; // slots 16..39: (frame%3)*MaxTimedScenes+scene
uint32_t splitScenes=0,frameSplits=1,frameScene=0;
uint64_t splitStarts[MaxTimedScenes]{},frameDraws=0;
SceGxmDepthStencilSurface depthFirst{},depthNext{}; // force store / force load+store copies

int timing_main(SceSize,void*) {
    while(sceKernelWaitSema(timingSema,1,nullptr)>=0 && !timingStop.load(std::memory_order_acquire)) {
        const TimedWait w=timedWaits[timedTail.load(std::memory_order_relaxed)%TimingQueue];
        SceGxmNotification n{w.address,w.value};
        sceGxmNotificationWait(&n);
        const uint64_t now=twv::vita::processTimeUs();
        auto& rec=timedFrames[w.frame%TimingRing];
        if(rec.frame.load(std::memory_order_acquire)==w.frame) {
            if(w.vertex)rec.vertexDone.store(now,std::memory_order_release);
            else rec.fragmentDone[w.scene].store(now,std::memory_order_release);
        }
        timedTail.fetch_add(1,std::memory_order_release);
    }
    return 0;
}
// Queued in GPU completion order: a frame's vertex notification before its fragment ones.
void timing_wait(uint64_t frame,uint32_t scene,bool vertex,const SceGxmNotification& n) {
    const uint32_t head=timedHead.load(std::memory_order_relaxed);
    if(head-timedTail.load(std::memory_order_acquire)>=TimingQueue){++timingQueueFull;return;}
    timedWaits[head%TimingQueue]={frame,scene,vertex,n.address,n.value};
    timedHead.store(head+1,std::memory_order_release);
    sceKernelSignalSema(timingSema,1);
}

void display_callback(const void* data) {
    const auto* d=static_cast<const DisplayData*>(data);
    const uint64_t start=timingOn?twv::vita::processTimeUs():0;
    SceDisplayFrameBuf fb{};
    fb.size=sizeof fb;fb.base=d->address;fb.pitch=renderStride;
    fb.pixelformat=SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;fb.width=renderWidth;fb.height=renderHeight;
    sceDisplaySetFrameBuf(&fb,SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStartMulti(1); // vitaGL's default swap interval
    if(timingOn && d->frame) {
        auto& rec=timedFrames[d->frame%TimingRing];
        if(rec.frame.load(std::memory_order_acquire)==d->frame) {
            rec.flipStart.store(start,std::memory_order_release);
            rec.flipEnd.store(twv::vita::processTimeUs(),std::memory_order_release);
        }
    }
}

// --- Cg compiler (SceShaccCg, the user's module) ---
std::mutex compileMutex; // the compiler keeps global state: one compile at a time
uint64_t compilerFingerprint=0;
bool compilerReady=false;
SceShaccCgSourceFile compileSource{};
SceShaccCgSourceFile* open_source(const char*,const SceShaccCgSourceLocation*,const SceShaccCgCompileOptions*,const char**) {
    return &compileSource;
}
void* compiler_malloc(unsigned int size){return std::malloc(size);}
void compiler_free(void* p){std::free(p);}
bool load_compiler() {
    const char* paths[]={"ur0:/data/libshacccg.suprx","ur0:data/external/libshacccg.suprx"};
    for(const char* path:paths) {
        FILE* file=std::fopen(path,"rb");
        if(!file)continue;
        std::vector<unsigned char> bytes;
        unsigned char chunk[16384];size_t n;
        while((n=std::fread(chunk,1,sizeof chunk,file))>0)bytes.insert(bytes.end(),chunk,chunk+n);
        std::fclose(file);
        const SceUID module=sceKernelLoadStartModule(path,0,nullptr,0,nullptr,nullptr);
        if(module<0){ok(module,"sceKernelLoadStartModule libshacccg");continue;}
        compilerFingerprint=compiler_fingerprint(bytes.data(),bytes.size());
        // Same compiler set-up as vitaGL (vitaShaRK shark_init): extensions on, default allocator.
        twv::vita::telemetryEvent("gxm_shacccg_ext",sceShaccCgExtEnableExtensions());
        sceShaccCgSetDefaultAllocator(compiler_malloc,compiler_free);
        char text[96];std::snprintf(text,sizeof text,"%s %016llx",path,static_cast<unsigned long long>(compilerFingerprint));
        twv::vita::telemetryText("gxm_shader_compiler",text);
        return compilerReady=true;
    }
    return false;
}
std::vector<unsigned char> compile_gxp(uint32_t stage,const char* source) {
    std::lock_guard<std::mutex> lock(compileMutex);
    compileSource.fileName="<built-in>";compileSource.text=source;compileSource.size=uint32_t(std::strlen(source));
    SceShaccCgCompileOptions options;
    sceShaccCgInitializeCompileOptions(&options);
    // vitaGL's settings (vitaShaRK SHARK_OPT_FAST, fastmath, fastint, LOG_ERRORS warnings);
    // gxm_shader_cache.hpp CompileOptionWords lists them for the cache key.
    options.mainSourceFile="<built-in>";
    options.targetProfile=stage==StageVertex?SCE_SHACCCG_PROFILE_VP:SCE_SHACCCG_PROFILE_FP;
    options.entryFunctionName="main";
    options.macroDefinitions=nullptr;
    options.optimizationLevel=3;options.useFastmath=1;options.useFastprecision=0;options.useFastint=1;
    options.useFx=1;options.locale=SCE_SHACCCG_ENGLISH;
    options.warningLevel=3;options.performanceWarnings=1;options.pedantic=0;
    SceShaccCgCallbackList callbacks;
    sceShaccCgInitializeCallbackList(&callbacks,SCE_SHACCCG_TRIVIAL);
    callbacks.openFile=open_source;
    const SceShaccCgCompileOutput* out=sceShaccCgCompileProgram(&options,&callbacks,0);
    std::vector<unsigned char> blob;
    if(out) {
        for(int i=0;i<out->diagnosticCount;++i)if(out->diagnostics[i].level==SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR) {
            char text[240];const auto* loc=out->diagnostics[i].location;
            std::snprintf(text,sizeof text,"line %u: %s",loc?unsigned(loc->lineNumber):0u,
                          out->diagnostics[i].message?out->diagnostics[i].message:"?");
            twv::vita::telemetryText("shader_compile_failure",text);break;
        }
        if(out->programData && out->programSize)blob.assign(out->programData,out->programData+out->programSize);
        sceShaccCgDestroyCompileOutput(out);
    }
    return blob;
}
bool valid_gxp(const std::vector<unsigned char>& blob) {
    if(blob.size()<32)return false;
    const auto* program=reinterpret_cast<const SceGxmProgram*>(blob.data());
    return sceGxmProgramCheck(program)==0 && sceGxmProgramGetSize(program)<=blob.size();
}
void make_dirs(uint32_t stage,uint64_t key) {
    char path[160];
    mkdir(GxpRoot,0777);
    std::snprintf(path,sizeof path,"%s/%s",GxpRoot,stage==StageVertex?"v":"f");mkdir(path,0777);
    std::snprintf(path,sizeof path,"%s/%s/%02x",GxpRoot,stage==StageVertex?"v":"f",unsigned(key>>56));mkdir(path,0777);
}

struct Stage {
    uint32_t stage=0;
    std::vector<unsigned char> gxp;
    SceGxmShaderPatcherId id=nullptr;
    const SceGxmProgram* program()const {return reinterpret_cast<const SceGxmProgram*>(gxp.data());}
};
std::unordered_map<uint64_t,std::unique_ptr<Stage>> stages;
Stage* acquire_stage(uint32_t stage,const char* source) {
    if(!compilerReady || !source)return nullptr;
    const uint64_t key=cache_key(stage,compilerFingerprint,source);
    if(auto found=stages.find(key);found!=stages.end()){++stageMemoryHits;return found->second.get();}
    twv::vgl::capture_shader_source(stage,source);
    const uint64_t start=twv::vita::processTimeUs();
    const uint64_t sourceHash=source_hash(source);
    char path[192];cache_path(path,sizeof path,GxpRoot,stage,key);
    auto entry=std::make_unique<Stage>();entry->stage=stage;
    if(FILE* file=std::fopen(path,"rb")) {
        std::vector<unsigned char> bytes;unsigned char chunk[8192];size_t n;
        while((n=std::fread(chunk,1,sizeof chunk,file))>0 && bytes.size()<(1u<<20))bytes.insert(bytes.end(),chunk,chunk+n);
        std::fclose(file);
        uint32_t size=0;
        if(const auto* gxp=parse_cache_file(bytes.data(),bytes.size(),stage,key,sourceHash,size)) {
            entry->gxp.assign(gxp,gxp+size);
            if(valid_gxp(entry->gxp))++stageDiskHits;
            else {entry->gxp.clear();++stageDiskRejects;}
        } else ++stageDiskRejects;
    }
    if(entry->gxp.empty()) {
        entry->gxp=compile_gxp(stage,source);
        if(!valid_gxp(entry->gxp)) {
            ++stageFailures;twv::vita::telemetryText("gxm_stage_failure",stage==StageVertex?"vertex":"fragment");
            return nullptr;
        }
        ++stageCompiles;
        make_dirs(stage,key);
        if(FILE* file=std::fopen(path,"wb")) {
            const auto header=make_header(stage,key,sourceHash,uint32_t(entry->gxp.size()));
            const bool written=std::fwrite(&header,sizeof header,1,file)==1 &&
                std::fwrite(entry->gxp.data(),1,entry->gxp.size(),file)==entry->gxp.size();
            if(std::fclose(file)!=0 || !written){std::remove(path);twv::vita::telemetryText("gxm_cache_write_failure",path);}
        }
    }
    if(!ok(sceGxmShaderPatcherRegisterProgram(patcher,entry->program(),&entry->id),"RegisterProgram"))return nullptr;
    twv::vgl::observe_shader_compile(twv::vita::processTimeUs()-start);
    return stages.emplace(key,std::move(entry)).first->second.get();
}

// --- programs ---
constexpr const char* AttributeNames[TwvVertexAttributeCount]={"position","color0","color1","tc0","tc1","tc2","tc3",
    "tc4","tc5","tc6","tc7","normal","pn_index"};
struct UniformRef {const SceGxmProgramParameter* param=nullptr;bool fragment=false;bool buffer1=false;};
struct Attribute {int attr=-1;uint16_t regIndex=0;uint8_t components=4;};
struct VertexVariant {
    TwvVitaVertexLayout layout;
    std::array<uint8_t,TwvVertexAttributeCount> formats,counts; // G2 packed formats (0 = F32)
    SceGxmVertexProgram* program;bool generic;
};
struct FragmentVariant {uint32_t blend;SceGxmFragmentProgram* program;};
struct Program {
    Stage* vs=nullptr;Stage* fs=nullptr;
    uint32_t references=0;
    std::pair<std::string,std::string> key;
    std::array<UniformRef,4> uniforms{};         // proj_rows, regs, konst, fog
    std::array<UniformRef,11> vertexUniforms{};  // pn_pos .. texgen_rows
    std::array<int,8> textureIndex{{-1,-1,-1,-1,-1,-1,-1,-1}};
    std::vector<Attribute> attributes;
    std::vector<uint8_t> vertexDefaults,fragmentDefaults;
    std::vector<VertexVariant> vertexVariants;
    std::vector<FragmentVariant> fragmentVariants;
    int discard=-1;uint32_t fragmentBytes=0; // GPU split receipts: alpha test, GXP size (on first use)
    // G3 uniform split: vertex uniform buffer 1 (empty when the program has none), the lighting
    // values its bytes were built from, and this frame's copy of it in the ring.
    std::vector<uint8_t> buffer1;
    std::vector<float> buffer1Source;uint32_t buffer1Lights=UINT32_MAX;
    const void* buffer1Copy=nullptr;uint64_t buffer1Frame=0;
    // G3 step 2 (--vita-gxm-fragment-split): the same for fragment uniform buffer 1 (TEV registers,
    // konst colours, fog).
    std::vector<uint8_t> fragmentBuffer1;
    std::vector<float> fragmentBuffer1Source;
    const void* fragmentBuffer1Copy=nullptr;uint64_t fragmentBuffer1Frame=0;
};
std::vector<Program> programs(1);
std::vector<uint32_t> freeProgramIds;
std::map<std::pair<std::string,std::string>,uint32_t> programCache;
struct PatchedRelease {SceGxmVertexProgram* vertex;SceGxmFragmentProgram* fragment;};
RetireList<PatchedRelease> retiredPrograms;

UniformRef find_uniform(const Program& p,const char* name) {
    if(const auto* param=sceGxmProgramFindParameterByName(p.vs->program(),name))
        if(sceGxmProgramParameterGetCategory(param)==SCE_GXM_PARAMETER_CATEGORY_UNIFORM)
            return {param,false,sceGxmProgramParameterGetContainerIndex(param)==1};
    if(const auto* param=sceGxmProgramFindParameterByName(p.fs->program(),name))
        if(sceGxmProgramParameterGetCategory(param)==SCE_GXM_PARAMETER_CATEGORY_UNIFORM)
            return {param,true,sceGxmProgramParameterGetContainerIndex(param)==1};
    return {};
}
// G3 step 2: bytes of a program's uniform buffer 1 (its last uniform's end, in 32-bit words, at
// least 1 KiB), 0 when it has none.
size_t uniform_buffer1_bytes(const SceGxmProgram* program) {
    size_t words=0;bool found=false;
    const unsigned count=sceGxmProgramGetParameterCount(program);
    for(unsigned i=0;i<count;++i) {
        const auto* param=sceGxmProgramGetParameter(program,i);
        if(sceGxmProgramParameterGetCategory(param)!=SCE_GXM_PARAMETER_CATEGORY_UNIFORM ||
           sceGxmProgramParameterGetContainerIndex(param)!=1)continue;
        found=true;
        words=std::max<size_t>(words,sceGxmProgramParameterGetResourceIndex(param)+
            size_t(sceGxmProgramParameterGetArraySize(param))*sceGxmProgramParameterGetComponentCount(param));
    }
    return found?std::max<size_t>(1024,(words*4+15)&~size_t(15)):0;
}
bool describe_program(Program& p) {
    const char* names[]={"proj_rows","regs","konst","fog"};
    for(int i=0;i<4;++i)p.uniforms[i]=find_uniform(p,names[i]);
    const char* vertexNames[]={"pn_pos","pn_nrm","light_pos","light_dir","light_color",
        "light_cos","light_dist","channel_mat","channel_amb","light_masks","texgen_rows"};
    for(int i=0;i<11;++i)p.vertexUniforms[i]=find_uniform(p,vertexNames[i]);
    for(int i=0;i<8;++i) {
        char name[16];std::snprintf(name,sizeof name,"tex%d",i);
        const auto* param=sceGxmProgramFindParameterByName(p.fs->program(),name);
        if(param && sceGxmProgramParameterGetCategory(param)==SCE_GXM_PARAMETER_CATEGORY_SAMPLER)
            p.textureIndex[i]=int(sceGxmProgramParameterGetResourceIndex(param));
    }
    const auto* vs=p.vs->program();
    const unsigned count=sceGxmProgramGetParameterCount(vs);
    for(unsigned i=0;i<count;++i) {
        const auto* param=sceGxmProgramGetParameter(vs,i);
        if(sceGxmProgramParameterGetCategory(param)!=SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)continue;
        Attribute a;
        const char* name=sceGxmProgramParameterGetName(param);
        for(unsigned k=0;k<TwvVertexAttributeCount;++k)if(name && std::strcmp(name,AttributeNames[k])==0)a.attr=int(k);
        a.regIndex=uint16_t(sceGxmProgramParameterGetResourceIndex(param));
        const unsigned components=sceGxmProgramParameterGetComponentCount(param);
        a.components=uint8_t(components && components<=4?components:4);
        if(a.attr<0){twv::vita::telemetryText("gxm_unknown_attribute",name?name:"?");return false;}
        p.attributes.push_back(a);
    }
    if(p.attributes.size()>15){twv::vita::telemetryText("gxm_program_failure","too many attributes");return false;}
    p.vertexDefaults.assign(sceGxmProgramGetDefaultUniformBufferSize(vs),0);
    p.fragmentDefaults.assign(sceGxmProgramGetDefaultUniformBufferSize(p.fs->program()),0);
    // G3: vertex uniform buffer 1 holds the uniforms whose container is 1, each at its resource index
    // (32-bit words). Its size covers the last one, with at least 1 KiB.
    size_t buffer1Words=0;
    bool buffer1=false;
    for(unsigned i=0;i<count;++i) {
        const auto* param=sceGxmProgramGetParameter(vs,i);
        if(sceGxmProgramParameterGetCategory(param)!=SCE_GXM_PARAMETER_CATEGORY_UNIFORM ||
           sceGxmProgramParameterGetContainerIndex(param)!=1)continue;
        buffer1=true;
        buffer1Words=std::max<size_t>(buffer1Words,sceGxmProgramParameterGetResourceIndex(param)+
            size_t(sceGxmProgramParameterGetArraySize(param))*sceGxmProgramParameterGetComponentCount(param));
    }
    if(buffer1) {
        if(!(twv_gpu_features()&TwvGpuFeatureUniformSplit) || buffer1Words*4>4096) {
            twv::vita::telemetryText("gxm_program_failure","uniform buffer 1 unexpected or too large");return false;
        }
        p.buffer1.assign(std::max<size_t>(1024,(buffer1Words*4+15)&~size_t(15)),0);
    }
    if(const size_t bytes=uniform_buffer1_bytes(p.fs->program())) {
        if(!(twv_gpu_features()&TwvGpuFeatureFragmentSplit) || bytes>4096) {
            twv::vita::telemetryText("gxm_program_failure","fragment uniform buffer 1 unexpected or too large");return false;
        }
        p.fragmentBuffer1.assign(bytes,0);
    }
    return true;
}
SceGxmAttributeFormat attribute_format(uint8_t format) {
    switch(format) {
    case TwvFormatS16:return SCE_GXM_ATTRIBUTE_FORMAT_S16;
    case TwvFormatU16:return SCE_GXM_ATTRIBUTE_FORMAT_U16;
    case TwvFormatS8:return SCE_GXM_ATTRIBUTE_FORMAT_S8;
    case TwvFormatU8:return SCE_GXM_ATTRIBUTE_FORMAT_U8;
    case TwvFormatU8N:return SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    default:return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    }
}
const VertexVariant* vertex_variant(Program& p,const TwvVitaDrawState& s) {
    const auto& layout=s.vertex_layout;
    std::array<uint8_t,TwvVertexAttributeCount> formats{},counts{};
    std::memcpy(formats.data(),s.vertex_formats,formats.size());
    std::memcpy(counts.data(),s.vertex_counts,counts.size());
    for(const auto& v:p.vertexVariants)
        if(std::memcmp(&v.layout,&layout,sizeof layout)==0 && v.formats==formats && v.counts==counts)return &v;
    SceGxmVertexAttribute attributes[16];SceGxmVertexStream streams[2];
    unsigned n=0;bool generic=false,packed=false;
    for(const auto& a:p.attributes) {
        auto& out=attributes[n++];
        if(layout.offsets[a.attr]!=TwvVertexAttributeAbsent) {
            // Packed integer attributes are scaled in the shader (exact powers of two); U8N
            // colours are normalised by the GPU. Missing components read 0 (and w 1).
            out.streamIndex=0;out.offset=uint16_t(layout.offsets[a.attr]);
            out.format=uint8_t(attribute_format(formats[a.attr]));
            out.componentCount=uint8_t(formats[a.attr]?counts[a.attr]:layout.components[a.attr]);
            packed|=formats[a.attr]!=TwvFormatF32;
        } else {
            // vitaGL feeds a disabled array from a zero-filled constant (stride 0).
            out.streamIndex=1;out.offset=0;out.componentCount=a.components;generic=true;
            out.format=SCE_GXM_ATTRIBUTE_FORMAT_F32;
        }
        out.regIndex=a.regIndex;
    }
    streams[0].stride=uint16_t(layout.stride);streams[0].indexSource=SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    streams[1].stride=0;streams[1].indexSource=SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    SceGxmVertexProgram* program=nullptr;
    if(!ok(sceGxmShaderPatcherCreateVertexProgram(patcher,p.vs->id,attributes,n,streams,generic?2:1,&program),
           "CreateVertexProgram"))return nullptr;
    ++vertexVariants;if(generic)++genericAttributeVariants;if(packed)++packedVariants;
    p.vertexVariants.push_back({layout,formats,counts,program,generic});
    return &p.vertexVariants.back();
}
SceGxmFragmentProgram* fragment_variant(Program& p,const BlendKey& key) {
    const uint32_t packed=key.packed();
    for(const auto& f:p.fragmentVariants)if(f.blend==packed)return f.program;
    SceGxmBlendInfo blend{};
    blend.colorMask=key.colorMask;blend.colorFunc=key.colorFunc;blend.alphaFunc=key.alphaFunc;
    blend.colorSrc=key.colorSrc;blend.colorDst=key.colorDst;blend.alphaSrc=key.alphaSrc;blend.alphaDst=key.alphaDst;
    SceGxmFragmentProgram* program=nullptr;
    if(!ok(sceGxmShaderPatcherCreateFragmentProgram(patcher,p.fs->id,SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
           SCE_GXM_MULTISAMPLE_NONE,&blend,p.vs->program(),&program),"CreateFragmentProgram"))return nullptr;
    ++fragmentVariants;
    p.fragmentVariants.push_back({packed,program});
    return program;
}

// --- textures ---
struct Texture {
    bool live=false;
    uint32_t offset=0,bytes=0,width=0,height=0,levels=0;
    SceGxmTexture gxm{};
    twv::vgl::SamplerParams object{1,1,0,0}; // GL_LINEAR / GL_LINEAR / clamp, as created
    uint64_t lastUse=0;
};
std::vector<Texture> textures(1);
std::vector<uint32_t> freeTextureIds;
Texture missingTexture;
struct Range {uint32_t offset,bytes;};
RetireList<Range> retiredTextureMemory,retiredResident;
unsigned char* texture_data(const Texture& t){return static_cast<unsigned char*>(texturePool.base)+t.offset;}
bool allocate_texture_memory(Texture& t) {
    t.bytes=(texture_bytes(t.width,t.height,t.levels)+TextureAlignment-1)&~(TextureAlignment-1);
    if(!textureAllocator.allocate(t.bytes,TextureAlignment,t.offset)) {
        ++textureAllocFailures;
        if(textureAllocFailures<=4)twv::vita::telemetryEvent("gxm_texture_pool_full_bytes",t.bytes);
        return false;
    }
    return true;
}
void write_level(Texture& t,uint32_t level,uint32_t width,uint32_t height,const unsigned char* rgba) {
    const auto m=mip_level(t.width,t.height,level);
    if(level>=t.levels || width!=m.width || height!=m.height){++errors;twv::vita::telemetryText("gxm_error","texture level size");return;}
    unsigned char* base=texture_data(t)+m.offset;
    for(uint32_t y=0;y<height;++y)std::memcpy(base+size_t(y)*m.stride*4,rgba+size_t(y)*width*4,size_t(width)*4);
}
uint32_t create_texture(uint32_t width,uint32_t height,uint32_t levels,const unsigned char* chain) {
    if(!ready || !width || !height || !levels || width>4096 || height>4096 || levels>13) {
        twv::vita::telemetryText("texture_dimension_failure","texture exceeds actual GPU limit");return 0;
    }
    Texture t;t.width=width;t.height=height;t.levels=levels;
    if(!allocate_texture_memory(t))return 0;
    std::memset(texture_data(t),0,t.bytes);
    if(!ok(sceGxmTextureInitLinear(&t.gxm,texture_data(t),SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,width,height,levels),
           "TextureInitLinear")) {textureAllocator.release(t.offset,t.bytes);return 0;}
    t.live=true;
    if(chain) {
        size_t offset=0;uint32_t w=width,h=height;
        for(uint32_t level=0;level<levels;++level) {
            write_level(t,level,w,h,chain+offset);
            offset+=size_t(w)*h*4;w=w>1?w/2:1;h=h>1?h/2:1;
        }
    }
    uint32_t id;
    if(!freeTextureIds.empty()){id=freeTextureIds.back();freeTextureIds.pop_back();}
    else {id=uint32_t(textures.size());textures.emplace_back();}
    textures[id]=t;
    return id;
}
Texture* find_texture(uint32_t id) {return id && id<textures.size() && textures[id].live?&textures[id]:nullptr;}
// Sampler fields of control word 0 (psp2/gxm.h layout), written directly as vitaGL does.
// In Vita3K the official setters return success but leave repeat/mirror addressing on a
// linear texture as clamp (init probe gxm_probe_setter_mismatches: 60 of 108 combinations),
// which smeared every repeating texture; the direct fields match the vitaGL path exactly.
void apply_sampler(SceGxmTexture& texture,const SamplerWords& w,uint32_t levels) {
    texture.generic.min_filter=w.minFilter;
    texture.generic.mip_filter=w.mipFilter?1:0;
    texture.generic.mag_filter=w.magFilter;
    texture.generic.uaddr_mode=w.u;
    texture.generic.vaddr_mode=w.v;
    texture.generic.mip_count=((w.mips?levels:1)-1)&0xF;
    texture.generic.lod_bias=LodBiasZero;
}
void retire_texture_memory(const Texture& t) {
    if(t.lastUse>completedFrame)retiredTextureMemory.retire({t.offset,t.bytes},t.lastUse);
    else textureAllocator.release(t.offset,t.bytes);
}

// --- clears ---
Stage* clearVs=nullptr;Stage* clearFs=nullptr;
SceGxmVertexProgram* clearVertexProgram=nullptr;
std::array<SceGxmFragmentProgram*,16> clearFragmentPrograms{};
const SceGxmProgramParameter* clearDepthParam=nullptr;const SceGxmProgramParameter* clearColorParam=nullptr;
constexpr const char* ClearVertexSource=
    "float4 main(float2 position : POSITION, uniform float depth) : POSITION { return float4(position, depth, 1.0); }\n";
constexpr const char* ClearFragmentSource="float4 main(uniform float4 color) : COLOR { return color; }\n";

// --- per-draw state (re-set after every BeginScene) ---
struct Shadow {
    bool valid=false;
    Viewport viewport{};Region region{};
    uint32_t cull=0,depthFunc=0,depthWrite=0;
    const SceGxmVertexProgram* vertex=nullptr;const SceGxmFragmentProgram* fragment=nullptr;
} shadow;
void set_viewport(const Viewport& v) {
    if(shadow.valid && std::memcmp(&v,&shadow.viewport,sizeof v)==0)return;
    const auto s=scale_viewport(v,renderScaleX,renderScaleY);
    sceGxmSetViewport(ctx,s.xOffset,s.xScale,s.yOffset,s.yScale,s.zOffset,s.zScale);shadow.viewport=v;
}
void set_region(const Region& r) {
    if(shadow.valid && r.xMin==shadow.region.xMin && r.yMin==shadow.region.yMin && r.xMax==shadow.region.xMax &&
       r.yMax==shadow.region.yMax)return;
    const auto s=scale_region(r,renderScaleX,renderScaleY,renderWidth,renderHeight);
    sceGxmSetRegionClip(ctx,SCE_GXM_REGION_CLIP_OUTSIDE,s.xMin,s.yMin,s.xMax,s.yMax);shadow.region=r;
}
void set_depth(uint32_t func,uint32_t write) {
    if(!shadow.valid || func!=shadow.depthFunc) {
        sceGxmSetFrontDepthFunc(ctx,SceGxmDepthFunc(func));sceGxmSetBackDepthFunc(ctx,SceGxmDepthFunc(func));shadow.depthFunc=func;
    }
    if(!shadow.valid || write!=shadow.depthWrite) {
        sceGxmSetFrontDepthWriteEnable(ctx,SceGxmDepthWriteMode(write));sceGxmSetBackDepthWriteEnable(ctx,SceGxmDepthWriteMode(write));
        shadow.depthWrite=write;
    }
}
void set_cull(uint32_t cull) {
    if(shadow.valid && cull==shadow.cull)return;
    sceGxmSetCullMode(ctx,SceGxmCullMode(cull));shadow.cull=cull;
}
// G3 first step (--vita-gxm-uniform-reuse): a draw whose patched programs and default-uniform
// bytes equal the ones last reserved reuses that reservation, which the context keeps until the
// next one, instead of reserving and copying again (uncached ring memory). Setting that program
// again (even the same one), a BeginScene or a clear ends the reuse; the byte comparison makes it
// exact.
bool uniformReuse=false;
const SceGxmVertexProgram* reservedVertexProgram=nullptr;
const SceGxmFragmentProgram* reservedFragmentProgram=nullptr;
std::vector<uint8_t> reservedVertexBytes,reservedFragmentBytes;
uint64_t uniformVertexReuses=0,uniformFragmentReuses=0,uniformVertexUploads=0,uniformFragmentUploads=0;
// G3 (--vita-gxm-uniform-split): the lighting uniforms sit in vertex uniform buffer 1 (the shader
// generator binds them there). Each program keeps that buffer's bytes and the draw-state values they
// were built from; a draw with the same values reuses this frame's copy instead of rewriting and
// copying it. Like the reservations, the binding is set again after a program change, a BeginScene
// or a clear.
bool uniformSplit=false,uniformSplitVerify=false;
const void* boundBuffer1=nullptr;
uint64_t splitUploads=0,splitReuses=0,splitBinds=0,splitRebuilds=0,splitChecks=0,splitMismatches=0;
std::vector<uint8_t> splitScratch; // verify: the reused buffer rebuilt from the draw's own values
// G3 step 2 (--vita-gxm-fragment-split): the same for fragment uniform buffer 1.
bool fragmentSplit=false,fragmentSplitVerify=false;
const void* boundFragmentBuffer1=nullptr;
uint64_t fsplitUploads=0,fsplitReuses=0,fsplitBinds=0,fsplitRebuilds=0,fsplitChecks=0,fsplitMismatches=0;
void uniform_reservations_end() {
    reservedVertexProgram=nullptr;reservedFragmentProgram=nullptr;boundBuffer1=nullptr;boundFragmentBuffer1=nullptr;
}
void set_programs(const SceGxmVertexProgram* vertex,const SceGxmFragmentProgram* fragment) {
    if(!shadow.valid || vertex!=shadow.vertex) {
        sceGxmSetVertexProgram(ctx,vertex);shadow.vertex=vertex;reservedVertexProgram=nullptr;boundBuffer1=nullptr;
    }
    if(!shadow.valid || fragment!=shadow.fragment) {
        sceGxmSetFragmentProgram(ctx,fragment);shadow.fragment=fragment;reservedFragmentProgram=nullptr;
        boundFragmentBuffer1=nullptr;
    }
}
void finish_state() {shadow.valid=true;}

void* ring_alloc(size_t bytes,uint32_t alignment=16) {
    if(!ready || bytes>RingSlotBytes)return nullptr;
    const uint32_t offset=ring.allocate(uint32_t(bytes),alignment);
    if(offset==UINT32_MAX){++ringFailures;return nullptr;}
    return static_cast<unsigned char*>(ringMemory.base)+offset;
}
void draw_clear(float r,float g,float b,float a,float depth,unsigned mask,int x,int y,int w,int h) {
    if(!inScene || w<=0 || h<=0)return;
    const auto q=clear_quad(x,y,w,h,depth);
    auto* vertices=static_cast<float*>(ring_alloc(8*sizeof(float)));
    if(!vertices)return;
    const float v[8]={q.x0,q.yTop,q.x1,q.yTop,q.x0,q.yBottom,q.x1,q.yBottom};
    std::memcpy(vertices,v,sizeof v);
    const uint8_t colorMask=uint8_t((mask&1?MaskR|MaskG|MaskB:0)|(mask&2?MaskA:0));
    auto& fragment=clearFragmentPrograms[colorMask];
    if(!fragment) {
        SceGxmBlendInfo blend{};blend.colorMask=colorMask;blend.colorFunc=SCE_GXM_BLEND_FUNC_NONE;
        blend.alphaFunc=SCE_GXM_BLEND_FUNC_NONE;blend.colorSrc=SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
        blend.colorDst=SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.alphaSrc=SCE_GXM_BLEND_FACTOR_ONE;
        blend.alphaDst=SCE_GXM_BLEND_FACTOR_ZERO;
        if(!ok(sceGxmShaderPatcherCreateFragmentProgram(patcher,clearFs->id,SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
               SCE_GXM_MULTISAMPLE_NONE,&blend,clearVs->program(),&fragment),"CreateFragmentProgram clear"))return;
    }
    set_viewport(viewport(0,0,DisplayWidth,DisplayHeight));
    set_region(scissor_region(x,y,w,h));
    set_cull(CullNone);
    set_depth(depth_func(7),(mask&4)?SCE_GXM_DEPTH_WRITE_ENABLED:SCE_GXM_DEPTH_WRITE_DISABLED);
    set_programs(clearVertexProgram,fragment);
    finish_state();
    const bool colour=(mask&3)!=0;
    if(!colour) {
        sceGxmSetFrontFragmentProgramEnable(ctx,SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
        sceGxmSetBackFragmentProgramEnable(ctx,SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
    }
    uniform_reservations_end(); // the clear reserves its own default uniforms
    void* buffer=nullptr;
    if(ok(sceGxmReserveVertexDefaultUniformBuffer(ctx,&buffer),"ReserveVertexDefaultUniformBuffer clear"))
        sceGxmSetUniformDataF(buffer,clearDepthParam,0,1,&q.z);
    if(colour && ok(sceGxmReserveFragmentDefaultUniformBuffer(ctx,&buffer),"ReserveFragmentDefaultUniformBuffer clear")) {
        const float colourValue[4]={r,g,b,a};sceGxmSetUniformDataF(buffer,clearColorParam,0,4,colourValue);
    }
    sceGxmSetVertexStream(ctx,0,vertices);
    ok(sceGxmDraw(ctx,SCE_GXM_PRIMITIVE_TRIANGLES,SCE_GXM_INDEX_FORMAT_U16,quadIndices,6),"Draw clear");
    if(!colour) {
        sceGxmSetFrontFragmentProgramEnable(ctx,SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
        sceGxmSetBackFragmentProgramEnable(ctx,SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    }
    ++clears;
}

// --- per-unit sampler requests (gpu_api.h contract) ---
std::array<twv::vgl::SamplerParams,8> unitParams{};
uint32_t pendingMask=0;bool pendingFailure=false;

// --- frames ---
void collect_completed() {
    for(uint64_t f=completedFrame+1;f<=frameSerial;++f) {
        if(notifications[f%FrameRing::Slots]>=f)completedFrame=f;else break;
    }
    retiredTextureMemory.collect(completedFrame,[](const Range& r){textureAllocator.release(r.offset,r.bytes);});
    retiredResident.collect(completedFrame,[](const Range& r){residentAllocator.release(r.offset,r.bytes);});
    retiredPrograms.collect(completedFrame,[](const PatchedRelease& r) {
        if(r.vertex)sceGxmShaderPatcherReleaseVertexProgram(patcher,r.vertex);
        if(r.fragment)sceGxmShaderPatcherReleaseFragmentProgram(patcher,r.fragment);
    });
}
void wait_frame(uint64_t frame,uint64_t& counter) {
    if(!frame || completedFrame>=frame)return;
    const unsigned slot=unsigned(frame%FrameRing::Slots);
    if(notifications[slot]<frame) {
        ++counter;const uint64_t start=twv::vita::processTimeUs();
        SceGxmNotification n{&notifications[slot],unsigned(frame)};
        ok(sceGxmNotificationWait(&n),"NotificationWait");
        frameWaitUs+=twv::vita::processTimeUs()-start;
    }
    completedFrame=frame;
}
// GPU timing: sum every frame whose stamps are all in, oldest first. A frame still missing a
// stamp after TimingRing-4 newer frames (or overwritten) is counted as dropped.
uint64_t timed_done(uint64_t frame) {
    if(!frame)return 0;
    const auto& rec=timedFrames[frame%TimingRing];
    if(rec.frame.load(std::memory_order_acquire)!=frame)return 0;
    return rec.fragmentDone[rec.main.scenes-1].load(std::memory_order_acquire);
}
void timing_finalize() {
    for(uint64_t f=timedFinal+1;f<frameSerial;++f) {
        auto& rec=timedFrames[f%TimingRing];
        if(rec.frame.load(std::memory_order_acquire)!=f){++timingDropped;timedFinal=f;continue;}
        FrameTimes t=rec.main;
        for(unsigned k=0;k<MaxTimedScenes;++k)t.fragmentDone[k]=rec.fragmentDone[k].load(std::memory_order_acquire);
        t.vertexDone=rec.vertexDone.load(std::memory_order_acquire);
        t.flipStart=rec.flipStart.load(std::memory_order_acquire);
        t.flipEnd=rec.flipEnd.load(std::memory_order_acquire);
        if(!t.complete()) {
            if(frameSerial-f<TimingRing-4)break; // still on the GPU or in the display queue
            ++timingDropped;timedFinal=f;continue;
        }
        const auto& previous=timedFrames[(f-1)%TimingRing];
        const uint64_t previousFlip=previous.frame.load(std::memory_order_acquire)==f-1?
            previous.flipEnd.load(std::memory_order_acquire):0;
        timingWindow.add(frame_metrics(t,timed_done(f-1),timed_done(f-2),previousFlip),t.scenes,rec.segments.data());
        timedFinal=f;
    }
}

// --- table entries ---
int gxm_init() {
    if(ready)return 1;
    initializing=true;
    if(!load_compiler()) {
        twv::vita::telemetryText("gfx_init_failure","GXM backend needs ur0:data/libshacccg.suprx (extracted from your own console)");
        twv::vita::startupNoteProblem(twv::vita::StartupProblem::ShaderCompiler,nullptr,0,0);
        return 0;
    }
    SceGxmInitializeParams init{};
    init.flags=0;init.displayQueueMaxPendingCount=DisplayBuffers-1;init.displayQueueCallback=display_callback;
    init.displayQueueCallbackDataSize=sizeof(DisplayData);init.parameterBufferSize=SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if(!ok(sceGxmInitialize(&init),"sceGxmInitialize"))return 0;
    unsigned int fragmentUsseOffset=0;
    if(!gpu_block(vdmRing,"twv_gxm_vdm",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,SCE_GXM_MEMORY_ATTRIB_READ) ||
       !gpu_block(vertexRing,"twv_gxm_vertex_ring",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE,SCE_GXM_MEMORY_ATTRIB_READ) ||
       !gpu_block(fragmentRing,"twv_gxm_fragment_ring",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE,SCE_GXM_MEMORY_ATTRIB_READ) ||
       !usse_block(fragmentUsseRing,"twv_gxm_fragment_usse_ring",SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,false,&fragmentUsseOffset))
        return 0;
    contextHost=std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    SceGxmContextParams context{};
    context.hostMem=contextHost;context.hostMemSize=SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    context.vdmRingBufferMem=vdmRing.base;context.vdmRingBufferMemSize=SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    context.vertexRingBufferMem=vertexRing.base;context.vertexRingBufferMemSize=SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    context.fragmentRingBufferMem=fragmentRing.base;context.fragmentRingBufferMemSize=SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    context.fragmentUsseRingBufferMem=fragmentUsseRing.base;
    context.fragmentUsseRingBufferMemSize=SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    context.fragmentUsseRingBufferOffset=fragmentUsseOffset;
    if(!ok(sceGxmCreateContext(&context,&ctx),"sceGxmCreateContext"))return 0;
    const uint32_t features=twv_gpu_features();
    if(features&(TwvGpuFeatureScale75|TwvGpuFeatureScale67|TwvGpuFeatureScale50)) {
        renderWidth=features&TwvGpuFeatureScale75?720:features&TwvGpuFeatureScale67?640:480;
        renderHeight=features&TwvGpuFeatureScale75?408:features&TwvGpuFeatureScale67?368:272;
        renderStride=(renderWidth+63)&~63u;
        renderScaleX=float(renderWidth)/float(DisplayWidth);renderScaleY=float(renderHeight)/float(DisplayHeight);
        twv::vita::telemetryEvent("gxm_render_width",renderWidth);
        twv::vita::telemetryEvent("gxm_render_height",renderHeight);
    }
    SceGxmRenderTargetParams rt{};
    rt.flags=0;rt.width=renderWidth;rt.height=renderHeight;rt.scenesPerFrame=8; // room for EFB splits (G5)
    rt.multisampleMode=SCE_GXM_MULTISAMPLE_NONE;rt.multisampleLocations=0;rt.driverMemBlock=-1;
    if(!ok(sceGxmCreateRenderTarget(&rt,&target),"sceGxmCreateRenderTarget"))return 0;
    for(unsigned i=0;i<DisplayBuffers;++i) {
        char name[32];std::snprintf(name,sizeof name,"twv_gxm_display%u",i);
        if(!gpu_block(displayMemory[i],name,SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,DisplayBytes,SCE_GXM_MEMORY_ATTRIB_RW))return 0;
        std::memset(displayMemory[i].base,0,DisplayBytes);
        if(!ok(sceGxmColorSurfaceInit(&displaySurface[i],SCE_GXM_COLOR_FORMAT_A8B8G8R8,SCE_GXM_COLOR_SURFACE_LINEAR,
               SCE_GXM_COLOR_SURFACE_SCALE_NONE,SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,renderWidth,renderHeight,
               renderStride,displayMemory[i].base),"sceGxmColorSurfaceInit") ||
           !ok(sceGxmSyncObjectCreate(&displaySync[i]),"sceGxmSyncObjectCreate"))return 0;
    }
    // vitaGL's display depth/stencil: 32-bit float depth with mask bit, separate 8-bit stencil, linear.
    constexpr uint32_t depthBytes=DisplayStride*DisplayHeight*4,stencilBytes=DisplayStride*DisplayHeight;
    if(!gpu_block(depthMemory,"twv_gxm_depth",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,depthBytes+stencilBytes,SCE_GXM_MEMORY_ATTRIB_RW))return 0;
    if(!ok(sceGxmDepthStencilSurfaceInit(&depthSurface,SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8,SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR,
           renderStride,depthMemory.base,static_cast<unsigned char*>(depthMemory.base)+depthBytes),"sceGxmDepthStencilSurfaceInit"))return 0;
    sceGxmDepthStencilSurfaceSetBackgroundDepth(&depthSurface,1.0f);
    unsigned int vertexUsseOffset=0,patcherFragmentUsseOffset=0;
    if(!gpu_block(patcherBuffer,"twv_gxm_patcher",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,256*1024,SCE_GXM_MEMORY_ATTRIB_READ) ||
       !usse_block(patcherVertexUsse,"twv_gxm_patcher_vusse",256*1024,true,&vertexUsseOffset) ||
       !usse_block(patcherFragmentUsse,"twv_gxm_patcher_fusse",256*1024,false,&patcherFragmentUsseOffset))return 0;
    SceGxmShaderPatcherParams pp{};
    pp.hostAllocCallback=[](void*,unsigned int size){return std::malloc(size);};
    pp.hostFreeCallback=[](void*,void* mem){std::free(mem);};
    pp.bufferMem=patcherBuffer.base;pp.bufferMemSize=patcherBuffer.size;
    pp.vertexUsseMem=patcherVertexUsse.base;pp.vertexUsseMemSize=patcherVertexUsse.size;pp.vertexUsseOffset=vertexUsseOffset;
    pp.fragmentUsseMem=patcherFragmentUsse.base;pp.fragmentUsseMemSize=patcherFragmentUsse.size;
    pp.fragmentUsseOffset=patcherFragmentUsseOffset;
    if(!ok(sceGxmShaderPatcherCreate(&pp,&patcher),"sceGxmShaderPatcherCreate"))return 0;
    if(!gpu_block(texturePool,"twv_gxm_textures",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,TexturePoolBytes,SCE_GXM_MEMORY_ATTRIB_READ))return 0;
    textureAllocator.reset(texturePool.size);
    // Resident geometry: written once by the CPU at a cache store, read by the GPU until a
    // release is retired by fence. CDRAM, never read back by the CPU outside the oracles.
    if(!gpu_block(residentMemory,"twv_gxm_resident",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,ResidentPoolBytes,SCE_GXM_MEMORY_ATTRIB_READ))return 0;
    residentAllocator.reset(residentMemory.size);
    if(!gpu_block(ringMemory,"twv_gxm_frame_ring",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,RingSlotBytes*FrameRing::Slots,SCE_GXM_MEMORY_ATTRIB_READ))return 0;
    ring.reset(RingSlotBytes);
    // Static index data: linear 0..65535, the clear quad, a zero attribute, the quad table.
    if(!gpu_block(staticMemory,"twv_gxm_static",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                  65536*2+256+QuadTableQuads*12,SCE_GXM_MEMORY_ATTRIB_READ))return 0;
    linearIndices=static_cast<uint16_t*>(staticMemory.base);
    for(uint32_t i=0;i<65536;++i)linearIndices[i]=uint16_t(i);
    quadIndices=linearIndices+65536;
    const uint16_t quad[6]={0,1,2,2,1,3};std::memcpy(quadIndices,quad,sizeof quad);
    zeroAttribute=reinterpret_cast<float*>(quadIndices+8);std::memset(zeroAttribute,0,16*sizeof(float));
    quadTable=quadIndices+64; // byte 128: after the clear quad (12 B) and the zero attribute (bytes 16-79)
    for(uint32_t q=0;q<QuadTableQuads;++q) {
        const uint16_t v=uint16_t(q*4);
        const uint16_t tri[6]={v,uint16_t(v+1),uint16_t(v+2),uint16_t(v+2),uint16_t(v+3),v};
        std::memcpy(quadTable+q*6,tri,sizeof tri);
    }
    notifications=sceGxmGetNotificationRegion();
    for(unsigned i=0;i<FrameRing::Slots;++i){notifications[i]=0;notifications[VertexNotificationBase+i]=0;}
    if(twv_gpu_features()&TwvGpuFeatureTiming) {
        timingSema=sceKernelCreateSema("twv_gxm_timing",0,0,TimingQueue,nullptr);
        timingThread=timingSema<0?-1:sceKernelCreateThread("twv_gxm_timing",timing_main,64,16*1024,0,
                                                         0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */,nullptr);
        timingOn=timingThread>=0 && sceKernelStartThread(timingThread,0,nullptr)>=0;
        if(!timingOn){++errors;twv::vita::telemetryText("gxm_error","GPU timing monitor thread");}
        twv::vita::telemetryEvent("gxm_gpu_timing",timingOn);
    }
    uniformReuse=(twv_gpu_features()&TwvGpuFeatureUniformReuse)!=0;
    if(uniformReuse)twv::vita::telemetryEvent("gxm_uniform_reuse",1);
    uniformSplit=(twv_gpu_features()&TwvGpuFeatureUniformSplit)!=0;
    uniformSplitVerify=uniformSplit && (twv_gpu_features()&TwvGpuFeatureUniformSplitVerify)!=0;
    if(uniformSplit)twv::vita::telemetryEvent("gxm_uniform_split",uniformSplitVerify?2:1);
    fragmentSplit=(twv_gpu_features()&TwvGpuFeatureFragmentSplit)!=0;
    fragmentSplitVerify=fragmentSplit && (twv_gpu_features()&TwvGpuFeatureFragmentSplitVerify)!=0;
    if(fragmentSplit)twv::vita::telemetryEvent("gxm_fragment_split",fragmentSplitVerify?2:1);
    if(timingOn && (twv_gpu_features()&TwvGpuFeatureSplit)) {
        splitScenes=std::min<uint32_t>(twv_gpu_split_scenes(),MaxTimedScenes);
        for(unsigned i=0;i<FrameRing::Slots*MaxTimedScenes;++i)notifications[SceneNotificationBase+i]=0;
        depthFirst=depthNext=depthSurface;
        sceGxmDepthStencilSurfaceSetForceStoreMode(&depthFirst,SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        sceGxmDepthStencilSurfaceSetForceLoadMode(&depthNext,SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        sceGxmDepthStencilSurfaceSetForceStoreMode(&depthNext,SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        twv::vita::telemetryEvent("gxm_gpu_split_scenes",splitScenes);
    }
    ready=true;
    // Missing-texture stand-in (unit bound with no texture): 1x1 transparent black.
    missingTexture.width=missingTexture.height=missingTexture.levels=1;
    if(!allocate_texture_memory(missingTexture))return ready=false,0;
    std::memset(texture_data(missingTexture),0,missingTexture.bytes);
    sceGxmTextureInitLinear(&missingTexture.gxm,texture_data(missingTexture),SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,1,1,1);
    missingTexture.live=true;
    {   // Standing check for apply_sampler: official setters against the direct fields, every sampler
        // combination on a 64x64 linear texture with 4 levels. Logs the count and the first difference
        // (on the console this shows whether the real libgxm setters behave like Vita3K's).
        SceGxmTexture base{};
        sceGxmTextureInitLinear(&base,texturePool.base,SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,64,64,4);
        unsigned mismatches=0;
        for(int min=0;min<6;++min)for(int mag=0;mag<2;++mag)for(int s=0;s<3;++s)for(int t=0;t<3;++t) {
            const auto w=sampler_words(min,mag,s,t);
            SceGxmTexture direct=base,official=base;
            apply_sampler(direct,w,4);
            sceGxmTextureSetMinFilter(&official,SceGxmTextureFilter(w.minFilter));
            sceGxmTextureSetMipFilter(&official,SceGxmTextureMipFilter(w.mipFilter));
            sceGxmTextureSetMagFilter(&official,SceGxmTextureFilter(w.magFilter));
            sceGxmTextureSetUAddrMode(&official,SceGxmTextureAddrMode(w.u));
            sceGxmTextureSetVAddrMode(&official,SceGxmTextureAddrMode(w.v));
            sceGxmTextureSetMipmapCount(&official,w.mips?4:1);
            sceGxmTextureSetLodBias(&official,LodBiasZero);
            if(std::memcmp(&direct,&official,sizeof direct)==0)continue;
            if(!mismatches++) {
                uint32_t d[4],o[4];std::memcpy(d,&direct,sizeof d);std::memcpy(o,&official,sizeof o);
                char text[200];
                std::snprintf(text,sizeof text,"min=%d mag=%d s=%d t=%d direct=%08x,%08x,%08x,%08x official=%08x,%08x,%08x,%08x",
                    min,mag,s,t,unsigned(d[0]),unsigned(d[1]),unsigned(d[2]),unsigned(d[3]),
                    unsigned(o[0]),unsigned(o[1]),unsigned(o[2]),unsigned(o[3]));
                twv::vita::telemetryText("gxm_probe_setter_difference",text);
            }
        }
        twv::vita::telemetryEvent("gxm_probe_setter_mismatches",mismatches);
    }
    clearVs=acquire_stage(StageVertex,ClearVertexSource);clearFs=acquire_stage(StageFragment,ClearFragmentSource);
    if(!clearVs || !clearFs){ready=false;twv::vita::telemetryText("gfx_init_failure","GXM clear programs");return 0;}
    clearDepthParam=sceGxmProgramFindParameterByName(clearVs->program(),"depth");
    clearColorParam=sceGxmProgramFindParameterByName(clearFs->program(),"color");
    const auto* positionParam=sceGxmProgramFindParameterByName(clearVs->program(),"position");
    if(!clearDepthParam || !clearColorParam || !positionParam){ready=false;twv::vita::telemetryText("gfx_init_failure","GXM clear parameters");return 0;}
    SceGxmVertexAttribute clearAttribute{};
    clearAttribute.streamIndex=0;clearAttribute.offset=0;clearAttribute.format=SCE_GXM_ATTRIBUTE_FORMAT_F32;
    clearAttribute.componentCount=2;clearAttribute.regIndex=uint16_t(sceGxmProgramParameterGetResourceIndex(positionParam));
    SceGxmVertexStream clearStream{};clearStream.stride=8;clearStream.indexSource=SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if(!ok(sceGxmShaderPatcherCreateVertexProgram(patcher,clearVs->id,&clearAttribute,1,&clearStream,1,&clearVertexProgram),
           "CreateVertexProgram clear")){ready=false;return 0;}
    twv::vita::telemetryEvent("gfx_initialized",1);
    twv::vita::telemetryText("gfx_backend","direct GXM (G1): SceShaccCg Cg, own GXP cache, fenced frame ring");
    twv::vita::telemetryEvent("gxm_texture_pool_bytes",texturePool.size);
    twv::vita::telemetryEvent("gxm_resident_pool_bytes",residentMemory.size);
    twv::vita::telemetryEvent("gxm_frame_ring_bytes",ringMemory.size);
    initializing=false;
    // Start-up screen (ui/startup_screen.hpp) in the display buffer the first frame does not draw into
    // (frontBuffer): "Loading...", then a progress bar once a stage compiles instead of loading from
    // the GXP cache. It stays on screen until the first frame flips; frame and stage counters and the
    // warm-up receipts are untouched (drawing time: startup_progress_draw_us).
    static uint64_t compilesBefore;static uint32_t warmupDone,warmupTotal;
    compilesBefore=stageCompiles;
    twv::vita::startupProgressBegin(displayMemory[frontBuffer].base,0);
    twv::vgl::prepare_shader_cache(twv::vgl::shader_bundle_path(),
        [](uint32_t stage,const char* source)->uint32_t{return acquire_stage(stage,source)?1u:0u;},[](uint32_t){},
        [](uint32_t done,uint32_t total){
            warmupDone=done;warmupTotal=total;
            twv::vita::startupProgressUpdate(done,total,stageCompiles>compilesBefore);
        });
    twv::vita::startupProgressEnd(warmupDone,warmupTotal);
    twv::vgl::begin_shader_source_capture("ux0:data/twilight-vita/shader_sources");
    return 1;
}
void gxm_shutdown() {
    if(!ready)return;
    if(inScene){sceGxmEndScene(ctx,nullptr,nullptr);inScene=false;}
    sceGxmDisplayQueueFinish();
    sceGxmFinish(ctx);
    if(timingOn) { // every queued notification has been written: the monitor drains, then stops
        timingStop.store(true,std::memory_order_release);sceKernelSignalSema(timingSema,1);
        SceUInt timeout=200000;sceKernelWaitThreadEnd(timingThread,nullptr,&timeout);
        timingOn=false;
    }
    ready=false; // process-lifetime pools end with the app, as for vitaGL
}
void gxm_begin() {
    if(!ready || inScene)return;
    const uint64_t frame=++frameSerial;
    const uint64_t waitedBefore=frameWaitUs;
    wait_frame(frame>FrameRing::Slots?frame-FrameRing::Slots:0,frameWaits);
    if(timingOn) {
        auto& rec=timedFrames[frame%TimingRing];
        rec.frame.store(0,std::memory_order_release);
        for(auto& done:rec.fragmentDone)done.store(0,std::memory_order_relaxed);
        rec.vertexDone.store(0,std::memory_order_relaxed);
        rec.flipStart.store(0,std::memory_order_relaxed);rec.flipEnd.store(0,std::memory_order_relaxed);
        rec.main=FrameTimes{};rec.segments={};
        rec.main.begin=twv::vita::processTimeUs();rec.main.fenceWait=frameWaitUs-waitedBefore;
        rec.frame.store(frame,std::memory_order_release);
    }
    collect_completed();
    ring.begin(frame);
    frameSplits=splitScenes?split_points(frameDraws,splitScenes,splitStarts):1;
    frameScene=0;frameDraws=0;
    if(!ok(sceGxmBeginScene(ctx,0,target,nullptr,nullptr,displaySync[backBuffer],&displaySurface[backBuffer],
                            frameSplits>1?&depthFirst:&depthSurface),"sceGxmBeginScene"))return;
    inScene=true;shadow.valid=false;uniform_reservations_end();
    // Physical pillarbox and depth reset, as the vitaGL backend's full-screen glClear.
    draw_clear(0,0,0,1,1.f,7,0,0,DisplayWidth,DisplayHeight);
}
int gxm_end(const char* capture) {
    if(!ready || !inScene)return 0;
    const uint64_t frame=frameSerial;
    SceGxmNotification done{&notifications[frame%FrameRing::Slots],unsigned(frame)};
    SceGxmNotification vertexDone{&notifications[VertexNotificationBase+frame%FrameRing::Slots],unsigned(frame)};
    auto& timed=timedFrames[frame%TimingRing];
    if(timingOn)timed.main.submit[timed.main.scenes-1]=twv::vita::processTimeUs();
    ok(sceGxmEndScene(ctx,timingOn?&vertexDone:nullptr,&done),"sceGxmEndScene");
    if(timingOn) {
        timing_wait(frame,0,true,vertexDone);
        timing_wait(frame,timed.main.scenes-1,false,done);
    }
    inScene=false;++frames;
    if(capture) {
        wait_frame(frame,captureWaits);
        const unsigned char* pixels=static_cast<const unsigned char*>(displayMemory[backBuffer].base);
        std::vector<unsigned char> copy(pixels,pixels+DisplayBytes);
        twv::gpu::write_png_rgba(capture,copy.data(),renderWidth,renderHeight,renderStride*4,false);
    }
    const uint64_t presentStart=twv::vita::processTimeUs();
    sceGxmPadHeartbeat(&displaySurface[backBuffer],displaySync[backBuffer]);
    DisplayData data{displayMemory[backBuffer].base,timingOn?frame:0};
    ok(sceGxmDisplayQueueAddEntry(displaySync[frontBuffer],displaySync[backBuffer],&data),"DisplayQueueAddEntry");
    frontBuffer=backBuffer;backBuffer=(backBuffer+1)%DisplayBuffers;
    const uint64_t presentEnd=twv::vita::processTimeUs();
    twv_vgl_profile_add(TWV_PROFILE_PRESENT,presentEnd-presentStart);
    if(timingOn) {
        timed.main.presentStart=presentStart;timed.main.presentEnd=presentEnd;
        timing_finalize();
    }
    return 1;
}
void gxm_pacing_report() {
    twv::vita::telemetryEvent("gfx_program_cache_hits",programCacheHits);
    twv::vita::telemetryEvent("gfx_unique_programs",uniquePrograms);
    // vitaGL's name, so tools/vita_run_report.py bounds its windows the same way (SceShaccCg compiles).
    twv::vita::telemetryEvent("gfx_shader_stage_compile_calls",stageCompiles);
    if(timingOn) { // per-frame means over the frames finalised since the last window
        const auto& w=timingWindow;
        twv::vita::telemetryEvent("gxm_gpu_timed_frames",w.frames);
        twv::vita::telemetryEvent("gxm_gpu_busy_us",w.mean(w.sum.gpuBusy));
        twv::vita::telemetryEvent("gxm_gpu_busy_max_us",w.maxGpuBusy);
        twv::vita::telemetryEvent("gxm_gpu_latency_us",w.mean(w.sum.gpuLatency));
        twv::vita::telemetryEvent("gxm_gpu_starved_us",w.mean(w.sum.gpuStarved));
        twv::vita::telemetryEvent("gxm_gpu_vertex_tail_us",w.mean(w.sum.vertexTail));
        twv::vita::telemetryEvent("gxm_present_wait_us",w.mean(w.sum.presentWait));
        twv::vita::telemetryEvent("gxm_present_wait_max_us",w.maxPresentWait);
        twv::vita::telemetryEvent("gxm_present_gpu_us",w.mean(w.sum.presentGpu));
        twv::vita::telemetryEvent("gxm_present_display_us",w.mean(w.sum.presentDisplay));
        twv::vita::telemetryEvent("gxm_flip_interval_us",w.flip_mean());
        twv::vita::telemetryEvent("gxm_display_lag_us",w.mean(w.sum.displayLag));
        twv::vita::telemetryEvent("gxm_fence_wait_us",w.mean(w.sum.fenceWait));
        twv::vita::telemetryEvent("gxm_gpu_timing_dropped",timingDropped);
        twv::vita::telemetryEvent("gxm_gpu_timing_queue_full",timingQueueFull);
        if(splitScenes) {
            twv::vita::telemetryEvent("gxm_gpu_split_window_scenes",w.report_scenes());
            for(unsigned k=0;k<w.report_scenes();++k) {
                char text[160];
                if(w.segment_text(k,text,sizeof text)>0)twv::vita::telemetryText("gxm_gpu_segment",text);
            }
        }
        timingWindow=TimingWindow{};
    }
}
void gxm_report() {
    twv::vita::telemetryEvent("gxm_frame_waits",frameWaits);
    twv::vita::telemetryEvent("gxm_frame_wait_us",frameWaitUs);
    twv::vita::telemetryEvent("gxm_capture_waits",captureWaits);
    twv::vita::telemetryEvent("gxm_ring_peak_bytes",ring.peak());
    twv::vita::telemetryEvent("gxm_ring_failures",ringFailures);
    twv::vita::telemetryEvent("gxm_texture_pool_used",textureAllocator.used());
    twv::vita::telemetryEvent("gxm_texture_pool_peak",textureAllocator.peak());
    twv::vita::telemetryEvent("gxm_texture_pool_fragments",textureAllocator.fragments());
    twv::vita::telemetryEvent("gxm_texture_alloc_failures",textureAllocFailures);
    twv::vita::telemetryEvent("gxm_texture_copies",textureCopies);
    twv::vita::telemetryEvent("gxm_texture_copy_bytes",textureCopyBytes);
    twv::vita::telemetryEvent("gxm_texture_retired_pending",retiredTextureMemory.pending());
    twv::vita::telemetryEvent("gxm_missing_texture_binds",missingTextureBinds);
    twv::vita::telemetryEvent("gxm_stage_compiles",stageCompiles);
    twv::vita::telemetryEvent("gxm_stage_disk_hits",stageDiskHits);
    twv::vita::telemetryEvent("gxm_stage_disk_rejects",stageDiskRejects);
    twv::vita::telemetryEvent("gxm_stage_memory_hits",stageMemoryHits);
    twv::vita::telemetryEvent("gxm_stage_failures",stageFailures);
    twv::vita::telemetryEvent("gxm_vertex_programs",vertexVariants);
    twv::vita::telemetryEvent("gxm_fragment_programs",fragmentVariants);
    twv::vita::telemetryEvent("gxm_generic_attribute_programs",genericAttributeVariants);
    twv::vita::telemetryEvent("gxm_uniform_reserve_failures",uniformReserveFailures);
    twv::vita::telemetryEvent("gxm_draws_skipped",drawsSkipped);
    twv::vita::telemetryEvent("gxm_clears",clears);
    twv::vita::telemetryEvent("gxm_patcher_host_bytes",sceGxmShaderPatcherGetHostMemAllocated(patcher));
    twv::vita::telemetryEvent("gxm_patcher_buffer_bytes",sceGxmShaderPatcherGetBufferMemAllocated(patcher));
    twv::vita::telemetryEvent("gxm_patcher_vusse_bytes",sceGxmShaderPatcherGetVertexUsseMemAllocated(patcher));
    twv::vita::telemetryEvent("gxm_patcher_fusse_bytes",sceGxmShaderPatcherGetFragmentUsseMemAllocated(patcher));
    twv::vita::telemetryEvent("gxm_uniform_vertex_uploads",uniformVertexUploads);
    twv::vita::telemetryEvent("gxm_uniform_vertex_reuses",uniformVertexReuses);
    twv::vita::telemetryEvent("gxm_uniform_fragment_uploads",uniformFragmentUploads);
    twv::vita::telemetryEvent("gxm_uniform_fragment_reuses",uniformFragmentReuses);
    if(uniformSplit) {
        twv::vita::telemetryEvent("gxm_uniform_split_rebuilds",splitRebuilds); // lighting values changed
        twv::vita::telemetryEvent("gxm_uniform_split_uploads",splitUploads);   // buffer 1 copied to the ring
        twv::vita::telemetryEvent("gxm_uniform_split_reuses",splitReuses);     // this frame's copy reused
        twv::vita::telemetryEvent("gxm_uniform_split_binds",splitBinds);
        if(uniformSplitVerify) {
            twv::vita::telemetryEvent("gxm_uniform_split_checks",splitChecks);
            twv::vita::telemetryEvent("gxm_uniform_split_mismatches",splitMismatches);
        }
    }
    if(fragmentSplit) {
        twv::vita::telemetryEvent("gxm_fragment_split_rebuilds",fsplitRebuilds);
        twv::vita::telemetryEvent("gxm_fragment_split_uploads",fsplitUploads);
        twv::vita::telemetryEvent("gxm_fragment_split_reuses",fsplitReuses);
        twv::vita::telemetryEvent("gxm_fragment_split_binds",fsplitBinds);
        if(fragmentSplitVerify) {
            twv::vita::telemetryEvent("gxm_fragment_split_checks",fsplitChecks);
            twv::vita::telemetryEvent("gxm_fragment_split_mismatches",fsplitMismatches);
        }
    }
    twv::vita::telemetryEvent("gxm_resident_used",residentAllocator.used());
    twv::vita::telemetryEvent("gxm_resident_peak",residentAllocator.peak());
    twv::vita::telemetryEvent("gxm_resident_allocs",residentAllocs);
    twv::vita::telemetryEvent("gxm_resident_failures",residentFailures);
    twv::vita::telemetryEvent("gxm_resident_releases",residentReleases);
    twv::vita::telemetryEvent("gxm_resident_retired_pending",retiredResident.pending());
    twv::vita::telemetryEvent("gxm_draws_triangles",primitiveDraws[0]);
    twv::vita::telemetryEvent("gxm_draws_strips",primitiveDraws[1]);
    twv::vita::telemetryEvent("gxm_draws_fans",primitiveDraws[2]);
    twv::vita::telemetryEvent("gxm_draws_quads",primitiveDraws[3]);
    twv::vita::telemetryEvent("gxm_packed_vertex_programs",packedVariants);
    twv::vita::telemetryEvent("gfx_draws",draws);
    twv::vita::telemetryEvent("gfx_errors",errors);
}
void* gxm_alloc(size_t size) {return ring_alloc(size);}
uint32_t gxm_vertex_is_vram(const void*) {return 0;}
uint32_t gxm_vertex_write_class(const void* address,size_t bytes) {
    const auto base=reinterpret_cast<uintptr_t>(ringMemory.base),p=reinterpret_cast<uintptr_t>(address);
    // The frame ring is uncached LPDDR this backend owns: the staging policy's verified class.
    if(ready && p>=base && bytes<=ringMemory.size && p-base<=ringMemory.size-bytes)
        return uint32_t(twv::vgl::VertexWriteClass::VerifiedUncachedRam);
    return uint32_t(twv::vgl::VertexWriteClass::Unknown);
}
void gxm_vertex_write_class_report() {}
uint32_t gxm_program(const char* vertex,const char* fragment) {
    if(!ready || !vertex || !fragment)return 0;
    std::pair<std::string,std::string> key{vertex,fragment};
    if(auto found=programCache.find(key);found!=programCache.end()) {
        ++programs[found->second].references;++programCacheHits;return found->second;
    }
    Program p;p.key=key;
    p.vs=acquire_stage(StageVertex,vertex);p.fs=acquire_stage(StageFragment,fragment);
    if(!p.vs || !p.fs || !describe_program(p))return 0;
    p.references=1;
    uint32_t id;
    if(!freeProgramIds.empty()){id=freeProgramIds.back();freeProgramIds.pop_back();}
    else {id=uint32_t(programs.size());programs.emplace_back();}
    programs[id]=std::move(p);
    programCache.emplace(std::move(key),id);++uniquePrograms;
    return id;
}
void gxm_program_delete(uint32_t id) {
    if(!id || id>=programs.size() || !programs[id].references || --programs[id].references)return;
    auto& p=programs[id];
    // Patched programs may still be used by frames in flight: release after this frame's fence.
    for(auto& v:p.vertexVariants)retiredPrograms.retire({v.program,nullptr},frameSerial);
    for(auto& f:p.fragmentVariants)retiredPrograms.retire({nullptr,f.program},frameSerial);
    programCache.erase(p.key);
    p=Program{};freeProgramIds.push_back(id);
}
void uniform4(Program& p,const UniformRef& u,int count,const float* values) {
    if(!u.param || count<=0)return;
    auto& buffer=u.fragment?(u.buffer1?p.fragmentBuffer1:p.fragmentDefaults):u.buffer1?p.buffer1:p.vertexDefaults;
    if(!buffer.empty())sceGxmSetUniformDataF(buffer.data(),u.param,0,unsigned(count)*4,values);
}
// G3: the lighting values a buffer-1 program reads, contiguous in the draw state (light_pos .. light_masks).
constexpr size_t LightBlockFloats=5*8*4+4*4+4*4+8*4;
static_assert(offsetof(TwvVitaDrawState,light_masks)+sizeof(TwvVitaDrawState::light_masks)-
              offsetof(TwvVitaDrawState,light_pos)==LightBlockFloats*sizeof(float));
// Builds (when the values changed) and copies (once per frame) the program's buffer 1, and binds it.
// False when the ring is full: the draw is skipped, as for a failed default-uniform reservation.
bool buffer1_bind(Program& p,const TwvVitaDrawState& s,const UniformRef* refs,const int* counts,
                  const float* const* values) {
    bool changed=p.buffer1Source.size()!=LightBlockFloats || p.buffer1Lights!=s.light_count ||
        std::memcmp(p.buffer1Source.data(),s.light_pos,LightBlockFloats*sizeof(float))!=0;
    if(!changed && uniformSplitVerify) {
        // The kept bytes must equal a fresh build from this draw's values; a mismatch is counted and
        // the buffer rebuilt.
        splitScratch=p.buffer1;
        for(int i=2;i<10;++i)if(refs[i].buffer1 && refs[i].param && counts[i]>0)
            sceGxmSetUniformDataF(splitScratch.data(),refs[i].param,0,unsigned(counts[i])*4,values[i]);
        ++splitChecks;
        if(splitScratch!=p.buffer1){++splitMismatches;changed=true;}
    }
    if(changed) {
        for(int i=2;i<10;++i)if(refs[i].buffer1)uniform4(p,refs[i],counts[i],values[i]);
        p.buffer1Source.assign(s.light_pos,s.light_pos+LightBlockFloats);
        p.buffer1Lights=s.light_count;++splitRebuilds;
    }
    if(changed || !p.buffer1Copy || p.buffer1Frame!=frameSerial) {
        void* copy=ring_alloc(p.buffer1.size());
        if(!copy){++uniformReserveFailures;uniform_reservations_end();return false;}
        std::memcpy(copy,p.buffer1.data(),p.buffer1.size());
        p.buffer1Copy=copy;p.buffer1Frame=frameSerial;++splitUploads;
    } else {
        ++splitReuses;
    }
    if(boundBuffer1!=p.buffer1Copy) {
        if(!ok(sceGxmSetVertexUniformBuffer(ctx,1,p.buffer1Copy),"SetVertexUniformBuffer 1")) {
            ++uniformReserveFailures;uniform_reservations_end();return false;
        }
        boundBuffer1=p.buffer1Copy;++splitBinds;
    }
    return true;
}
// G3 step 2: the fragment values a fragment buffer-1 program reads, contiguous in the draw state
// (color_regs, kcolors), and the uniforms they feed (p.uniforms[1..2]: regs, konst). Fog stays in
// the default buffer (the shader generator binds only these two).
constexpr size_t FragmentBlockFloats=16+16;
static_assert(offsetof(TwvVitaDrawState,kcolors)+sizeof(TwvVitaDrawState::kcolors)-
              offsetof(TwvVitaDrawState,color_regs)==FragmentBlockFloats*sizeof(float));
void fragment_buffer1_write(Program& p,const TwvVitaDrawState& s,std::vector<uint8_t>& out) {
    const float* values[]={s.color_regs,s.kcolors};
    for(int i=0;i<2;++i)if(p.uniforms[i+1].buffer1 && p.uniforms[i+1].fragment && p.uniforms[i+1].param)
        sceGxmSetUniformDataF(out.data(),p.uniforms[i+1].param,0,16,values[i]);
}
// As buffer1_bind, for the fragment stage.
bool fragment_buffer1_bind(Program& p,const TwvVitaDrawState& s) {
    bool changed=p.fragmentBuffer1Source.size()!=FragmentBlockFloats ||
        std::memcmp(p.fragmentBuffer1Source.data(),s.color_regs,FragmentBlockFloats*sizeof(float))!=0;
    if(!changed && fragmentSplitVerify) {
        splitScratch=p.fragmentBuffer1;
        fragment_buffer1_write(p,s,splitScratch);
        ++fsplitChecks;
        if(splitScratch!=p.fragmentBuffer1){++fsplitMismatches;changed=true;}
    }
    if(changed) {
        fragment_buffer1_write(p,s,p.fragmentBuffer1);
        p.fragmentBuffer1Source.assign(s.color_regs,s.color_regs+FragmentBlockFloats);
        ++fsplitRebuilds;
    }
    if(changed || !p.fragmentBuffer1Copy || p.fragmentBuffer1Frame!=frameSerial) {
        void* copy=ring_alloc(p.fragmentBuffer1.size());
        if(!copy){++uniformReserveFailures;uniform_reservations_end();return false;}
        std::memcpy(copy,p.fragmentBuffer1.data(),p.fragmentBuffer1.size());
        p.fragmentBuffer1Copy=copy;p.fragmentBuffer1Frame=frameSerial;++fsplitUploads;
    } else {
        ++fsplitReuses;
    }
    if(boundFragmentBuffer1!=p.fragmentBuffer1Copy) {
        if(!ok(sceGxmSetFragmentUniformBuffer(ctx,1,p.fragmentBuffer1Copy),"SetFragmentUniformBuffer 1")) {
            ++uniformReserveFailures;uniform_reservations_end();return false;
        }
        boundFragmentBuffer1=p.fragmentBuffer1Copy;++fsplitBinds;
    }
    return true;
}
// GPU split: end the current scene with its own notification and begin the next one on the
// same surfaces, depth reloaded. Draw state is set again after every BeginScene.
void split_scene() {
    const uint64_t frame=frameSerial;
    auto& rec=timedFrames[frame%TimingRing];
    SceGxmNotification done{&notifications[SceneNotificationBase+(frame%FrameRing::Slots)*MaxTimedScenes+frameScene],
                            unsigned(frame)};
    rec.main.submit[frameScene]=twv::vita::processTimeUs();
    if(!ok(sceGxmEndScene(ctx,nullptr,&done),"sceGxmEndScene split")){frameSplits=frameScene+1;return;}
    timing_wait(frame,frameScene,false,done);
    ++frameScene;rec.main.scenes=frameScene+1;
    inScene=ok(sceGxmBeginScene(ctx,0,target,nullptr,nullptr,displaySync[backBuffer],&displaySurface[backBuffer],&depthNext),
               "sceGxmBeginScene split");
    shadow.valid=false;uniform_reservations_end();
    if(!inScene) { // not expected with these surfaces: drop the frame, never leave its fence unwritten
        splitScenes=0;frameSplits=1;sceGxmFinish(ctx);
        notifications[frame%FrameRing::Slots]=unsigned(frame);
    }
}
void gxm_draw(const TwvVitaDrawState* s,const void* v,uint32_t count,const uint16_t* indices,uint32_t indexCount) {
    const uint32_t samplerMask=pendingMask;pendingMask=0;
    if(pendingFailure) {
        pendingFailure=false;
        twv::vita::telemetryText("gfx_sampler_unit_failure","failed current-draw sampler request; submission stopped");
        twv::vita::telemetryFlush();std::abort();
    }
    if(!inScene || !s || !s->program || !v || s->program>=programs.size() || !programs[s->program].references)return;
    auto& p=programs[s->program];
    const auto* vertex=vertex_variant(p,*s);
    auto* fragment=fragment_variant(p,blend_key(s->blend,s->subtract,s->src_factor,s->dst_factor,s->color_write,s->alpha_write));
    if(!vertex || !fragment){++drawsSkipped;return;}
    const uint16_t* drawIndices=indices;uint32_t drawCount=indexCount;
    SceGxmPrimitiveType primitive=SCE_GXM_PRIMITIVE_TRIANGLES;
    if(s->primitive==TwvPrimitiveStrip || s->primitive==TwvPrimitiveFan) {
        // G2 native primitives: the same triangles, in the same vertex order, as Aurora's index
        // expansion (strip parity and fan pivot follow the GL/GX conventions).
        if(count>65536){++drawsSkipped;return;}
        primitive=s->primitive==TwvPrimitiveStrip?SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:SCE_GXM_PRIMITIVE_TRIANGLE_FAN;
        drawIndices=linearIndices;drawCount=count;
    } else if(s->primitive==TwvPrimitiveQuads) {
        if(count/4>QuadTableQuads){++drawsSkipped;return;}
        drawIndices=quadTable;drawCount=count/4*6;
    } else if(!indexCount) {
        if(count>65536){++drawsSkipped;return;}
        drawIndices=linearIndices;drawCount=count; // GXM draws are always indexed
    }
    if(frameScene+1<frameSplits && frameDraws>=splitStarts[frameScene+1]) {
        split_scene();
        if(!inScene)return;
    }
    if(s->primitive<4)++primitiveDraws[s->primitive];
    set_viewport(viewport(s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3]));
    set_region(scissor_region(s->scissor[0],s->scissor[1],s->scissor[2],s->scissor[3]));
    set_cull(cull_mode(s->cull));
    if(s->depth_test)set_depth(depth_func(s->depth_func),s->depth_write?SCE_GXM_DEPTH_WRITE_ENABLED:SCE_GXM_DEPTH_WRITE_DISABLED);
    else set_depth(depth_func(7),SCE_GXM_DEPTH_WRITE_DISABLED);
    set_programs(vertex->program,fragment);
    finish_state();
    // Uniform values persist per program, as GL uniforms do; each draw writes its fields.
    uniform4(p,p.uniforms[0],4,s->projection);
    if(!p.uniforms[1].buffer1)uniform4(p,p.uniforms[1],4,s->color_regs);
    if(!p.uniforms[2].buffer1)uniform4(p,p.uniforms[2],4,s->kcolors);
    if(!p.uniforms[3].buffer1)uniform4(p,p.uniforms[3],2,s->fog);
    if(!p.fragmentBuffer1.empty() && !fragment_buffer1_bind(p,*s))return;
    if(s->gpu_vertex) {
        const float* values[]={s->pn_pos,s->pn_nrm,s->light_pos,s->light_dir,s->light_color,
            s->light_cos,s->light_dist,s->channel_mat,s->channel_amb,s->light_masks};
        const int n=int(s->light_count);
        const int counts[]={int(s->pn_vectors),int(s->pn_vectors),n,n,n,n,n,4,4,8};
        for(int i=0;i<10;++i)if(!p.vertexUniforms[i].buffer1)uniform4(p,p.vertexUniforms[i],counts[i],values[i]);
        if(s->texgen_vectors)uniform4(p,p.vertexUniforms[10],int(s->texgen_vectors),s->texgen_rows);
        if(!p.buffer1.empty() && !buffer1_bind(p,*s,p.vertexUniforms.data(),counts,values))return;
    }
    if(!p.vertexDefaults.empty()) {
        if(uniformReuse && reservedVertexProgram==vertex->program && reservedVertexBytes==p.vertexDefaults) {
            ++uniformVertexReuses;
        } else {
            void* buffer=nullptr;
            if(!ok(sceGxmReserveVertexDefaultUniformBuffer(ctx,&buffer),"ReserveVertexDefaultUniformBuffer")) {
                ++uniformReserveFailures;uniform_reservations_end();return;
            }
            std::memcpy(buffer,p.vertexDefaults.data(),p.vertexDefaults.size());
            ++uniformVertexUploads;
            if(uniformReuse){reservedVertexProgram=vertex->program;reservedVertexBytes=p.vertexDefaults;}
        }
    }
    if(!p.fragmentDefaults.empty()) {
        if(uniformReuse && reservedFragmentProgram==fragment && reservedFragmentBytes==p.fragmentDefaults) {
            ++uniformFragmentReuses;
        } else {
            void* buffer=nullptr;
            if(!ok(sceGxmReserveFragmentDefaultUniformBuffer(ctx,&buffer),"ReserveFragmentDefaultUniformBuffer")) {
                ++uniformReserveFailures;uniform_reservations_end();return;
            }
            std::memcpy(buffer,p.fragmentDefaults.data(),p.fragmentDefaults.size());
            ++uniformFragmentUploads;
            if(uniformReuse){reservedFragmentProgram=fragment;reservedFragmentBytes=p.fragmentDefaults;}
        }
    }
    for(unsigned i=0;i<8;++i)if(p.textureIndex[i]>=0) {
        Texture* t=find_texture(s->textures[i]);
        if(!t){t=&missingTexture;++missingTextureBinds;}
        SceGxmTexture texture=t->gxm;
        const auto params=(samplerMask&(1u<<i))?unitParams[i]:t->object;
        apply_sampler(texture,sampler_words(params.min,params.mag,params.s,params.t),t->levels);
        sceGxmSetFragmentTexture(ctx,unsigned(p.textureIndex[i]),&texture);
        t->lastUse=frameSerial;
    }
    sceGxmSetVertexStream(ctx,0,v);
    if(vertex->generic)sceGxmSetVertexStream(ctx,1,zeroAttribute);
    if(!ok(sceGxmDraw(ctx,primitive,SCE_GXM_INDEX_FORMAT_U16,drawIndices,drawCount),"sceGxmDraw"))return;
    ++draws;++frameDraws;
    if(splitScenes && timingOn) {
        if(p.discard<0){p.discard=sceGxmProgramIsDiscardUsed(p.fs->program())?1:0;p.fragmentBytes=sceGxmProgramGetSize(p.fs->program());}
        auto& seg=timedFrames[frameSerial%TimingRing].segments[frameScene];
        ++seg.draws;
        seg.triangles+=twv::gxm::triangles(primitive==SCE_GXM_PRIMITIVE_TRIANGLE_STRIP?1:primitive==SCE_GXM_PRIMITIVE_TRIANGLE_FAN?2:0,
                                           drawCount);
        seg.blended+=s->blend?1:0;seg.discard+=uint64_t(p.discard);seg.noDepth+=s->depth_test?0:1;
        seg.fragmentBytes+=p.fragmentBytes;
    }
}
uint32_t gxm_texture(uint32_t width,uint32_t height,uint32_t levels) {return create_texture(width,height,levels,nullptr);}
uint32_t gxm_texture_static_rgba(uint32_t width,uint32_t height,uint32_t levels,const void* bytes,size_t size) {
    if(!twv::vgl::complete_rgba_chain(width,height,levels,bytes,size))return 0;
    return create_texture(width,height,levels,static_cast<const unsigned char*>(bytes));
}
void gxm_texture_write(uint32_t id,uint32_t level,uint32_t width,uint32_t height,const void* rgba) {
    Texture* t=find_texture(id);
    if(!t || !rgba)return;
    if(t->lastUse>completedFrame) {
        // A frame in flight (or the one being built) reads the current storage: never rewrite
        // it in place (Vita3K can also keep stale copies of in-place rewrites). Copy-on-write.
        Texture copy=*t;
        if(!allocate_texture_memory(copy))return;
        std::memcpy(texture_data(copy),texture_data(*t),t->bytes);
        sceGxmTextureSetData(&copy.gxm,texture_data(copy));
        retiredTextureMemory.retire({t->offset,t->bytes},t->lastUse);
        copy.lastUse=0;*t=copy;
        ++textureCopies;textureCopyBytes+=t->bytes;
    }
    write_level(*t,level,width,height,static_cast<const unsigned char*>(rgba));
}
void gxm_texture_delete(uint32_t id) {
    Texture* t=find_texture(id);
    if(!t)return;
    retire_texture_memory(*t);
    *t=Texture{};freeTextureIds.push_back(id);
}
void gxm_texture_sampler(uint32_t id,int min,int mag,int s,int t) {
    if(Texture* texture=find_texture(id))texture->object=twv::vgl::canonical_sampler_params(min,mag,s,t);
}
int gxm_texture_sampler_unit(uint32_t unit,int min,int mag,int s,int t) {
    if(!ready || unit>=8){pendingFailure=true;return 0;}
    unitParams[unit]=twv::vgl::canonical_sampler_params(min,mag,s,t);pendingMask|=1u<<unit;return 1;
}
void gxm_clear(float r,float g,float b,float a,float depth,unsigned mask,int x,int y,int w,int h) {
    draw_clear(r,g,b,a,depth,mask,x,y,w,h);
}
// G2 resident geometry: CDRAM arena; a release may still be read by the frame being built (or
// frames in flight), so it is retired by the current frame's fence, never freed at once.
int gxm_resident_available() {return ready && residentMemory.base;}
void* gxm_resident_alloc(size_t bytes) {
    if(!ready || !bytes || bytes>residentMemory.size)return nullptr;
    const uint32_t size=(uint32_t(bytes)+ResidentAlignment-1)&~(ResidentAlignment-1);
    uint32_t offset=0;
    if(!residentAllocator.allocate(size,ResidentAlignment,offset)){++residentFailures;return nullptr;}
    residentSizes[offset]=size;++residentAllocs;
    return static_cast<unsigned char*>(residentMemory.base)+offset;
}
void gxm_resident_release(void* ptr) {
    if(!ptr)return;
    const auto offset=uint32_t(static_cast<unsigned char*>(ptr)-static_cast<unsigned char*>(residentMemory.base));
    const auto found=residentSizes.find(offset);
    if(found==residentSizes.end()){++errors;twv::vita::telemetryText("gxm_error","resident release of unknown pointer");return;}
    retiredResident.retire({offset,found->second},frameSerial);
    residentSizes.erase(found);++residentReleases;
}
const TwvGpuBackend gxmBackend{
    "gxm",gxm_init,gxm_shutdown,gxm_begin,gxm_end,gxm_pacing_report,gxm_report,gxm_alloc,
    gxm_vertex_is_vram,gxm_vertex_write_class,gxm_vertex_write_class_report,
    gxm_program,gxm_program_delete,gxm_draw,gxm_texture,gxm_texture_static_rgba,
    gxm_texture_write,gxm_texture_delete,gxm_texture_sampler,gxm_texture_sampler_unit,gxm_clear,
    gxm_resident_available,gxm_resident_alloc,gxm_resident_release,
    TwvGpuFeaturePrimitives|TwvGpuFeatureCompact|TwvGpuFeatureCompactColors|TwvGpuFeatureResident|
    TwvGpuFeatureTiming|TwvGpuFeatureSplit|TwvGpuFeatureScale75|TwvGpuFeatureScale67|TwvGpuFeatureScale50|
    TwvGpuFeatureUniformReuse|TwvGpuFeaturePipelineMemo|  // the memo is front-end only, offered with GXM
    TwvGpuFeatureUniformSplit|TwvGpuFeatureUniformSplitVerify|TwvGpuFeatureFragmentSplit|TwvGpuFeatureFragmentSplitVerify};
} // namespace
extern "C" const TwvGpuBackend* twv_gpu_backend_gxm(void) {return &gxmBackend;}
