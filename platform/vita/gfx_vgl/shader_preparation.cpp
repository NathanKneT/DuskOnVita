#include "shader_preparation.hpp"
#include "runtime/telemetry.hpp"
#include "runtime/run_control.hpp"
#include <cstdio>
#include <memory>
#include <new>
#include <sys/stat.h>
#ifndef TWV_VITA_SHADER_WARMUP
#define TWV_VITA_SHADER_WARMUP 1
#endif
#ifndef TWV_VITA_SHADER_SOURCE_CAPTURE
#define TWV_VITA_SHADER_SOURCE_CAPTURE 0
#endif
#ifndef TWV_VITA_APP_WARMUP_BUNDLE
#define TWV_VITA_APP_WARMUP_BUNDLE 0
#endif

namespace twv::vgl {
namespace {
#if TWV_VITA_SHADER_SOURCE_CAPTURE
bool captureSelected=false;
char captureRoot[256]{};
uint32_t captured=0;
size_t capturedBytes=0;
#endif
uint64_t compiles=0,compileUs=0,playCompiles=0,playCompileUs=0,playCompileMaxUs=0,playSlowCompiles=0;
// A cached GXP loads in about 10 ms on a Vita; a real Cg compile takes hundreds.
constexpr uint64_t SlowCompileUs=50000;
}
const char* shader_bundle_path() {
#if TWV_VITA_APP_WARMUP_BUNDLE
    struct stat info{};
    if(::stat(DataShaderBundle,&info)!=0 && ::stat(AppShaderBundle,&info)==0) {
        twv::vita::telemetryText("shader_warmup_source","app0 (packaged in the VPK)");
        return AppShaderBundle;
    }
#endif
    twv::vita::telemetryText("shader_warmup_source","data folder");
    return DataShaderBundle;
}
void prepare_shader_cache(const char* path,PrepareCompile compile,PrepareRelease release,PrepareProgress progress) {
#if TWV_VITA_SHADER_WARMUP
    FILE* file=std::fopen(path,"rb");
    if(!file) {twv::vita::telemetryText("shader_warmup","no optional source bundle");return;}
    const auto start=twv::vita::processTimeUs();
    twv::vita::telemetryText("shader_warmup_phase","validate before any native compilation");
    long size=-1;
    if(!std::fseek(file,0,SEEK_END))size=std::ftell(file);
    if(size<24 || size>long(ShaderBundleLimit) || std::fseek(file,0,SEEK_SET)) {
        std::fclose(file);twv::vita::telemetryText("shader_warmup_rejected","file size/seek");return;
    }
    std::unique_ptr<uint8_t[]> bytes(new(std::nothrow) uint8_t[size]);
    if(!bytes) {std::fclose(file);twv::vita::telemetryText("shader_warmup_rejected","bounded allocation unavailable");return;}
    const bool complete=std::fread(bytes.get(),1,size,file)==size_t(size) && std::fgetc(file)==EOF;
    std::fclose(file);
    std::unique_ptr<ShaderBundleView> held(new(std::nothrow) ShaderBundleView);
    if(!held) {twv::vita::telemetryText("shader_warmup_rejected","bounded allocation unavailable");return;}
    ShaderBundleView& view=*held;
    if(!complete || !parse_shader_bundle({bytes.get(),size_t(size)},view)) {
        twv::vita::telemetryText("shader_warmup_rejected","incomplete/invalid source bundle");return;
    }
    twv::vita::telemetryEvent("shader_warmup_sources",view.count);
    twv::vita::telemetryEvent("shader_warmup_bundle_bytes",size);
    twv::vita::telemetryText("shader_warmup_phase","prepare native cache before gameplay");
    uint32_t prepared=0,failed=0;
    for(uint32_t i=0;i<view.count;++i) {
        const auto& source=view.sources[i];
        const uint32_t shader=compile(source.stage,source.source);
        if(shader) {release(shader);++prepared;}
        else ++failed;
        if(progress)progress(i+1,view.count);
    }
    twv::vita::telemetryEvent("shader_warmup_prepared",prepared);
    twv::vita::telemetryEvent("shader_warmup_failed",failed);
    twv::vita::telemetryEvent("shader_warmup_elapsed_us",twv::vita::processTimeUs()-start);
    twv::vita::telemetryText("shader_warmup_phase","finished; original rendering handles uncached stages");
    twv::vita::telemetryFlush();
#else
    (void)path;(void)compile;(void)release;(void)progress;
#endif
}
void begin_shader_source_capture(const char* root) {
#if TWV_VITA_SHADER_SOURCE_CAPTURE
    captureRoot[0]=0;captured=0;capturedBytes=0;
    if(!captureSelected)return;
    ::mkdir(root,0777);
    const auto stamp=twv::vita::processTimeUs();
    for(unsigned attempt=0;attempt<16;++attempt) {
        const int length=std::snprintf(captureRoot,sizeof captureRoot,"%s/run-%016llX-%02u",root,
            static_cast<unsigned long long>(stamp),attempt);
        if(length<0 || size_t(length)>=sizeof captureRoot)break;
        if(!::mkdir(captureRoot,0777)) {
            twv::vita::telemetryText("shader_source_capture_root",captureRoot);return;
        }
    }
    captureRoot[0]=0;
    twv::vita::telemetryText("shader_source_capture_error","cannot create exclusive capture directory");
#else
    (void)root;
#endif
}
void capture_shader_source(uint32_t stage,const char* source) {
#if TWV_VITA_SHADER_SOURCE_CAPTURE
    if(!captureRoot[0])return;
    const size_t size=std::strlen(source);
    if((stage!=VertexSourceTag && stage!=FragmentSourceTag) || !size || size>65536 ||
       captured>=ShaderStageLimit || size>ShaderSourceLimit-capturedBytes) {
        captureRoot[0]=0;twv::vita::telemetryText("shader_source_capture_error","source capture bounds");return;
    }
    char path[288];std::snprintf(path,sizeof path,"%s/%04lu-%c.cg",captureRoot,
        static_cast<unsigned long>(captured),stage==VertexSourceTag?'v':'f');
    FILE* file=std::fopen(path,"wb");
    if(!file) {captureRoot[0]=0;twv::vita::telemetryText("shader_source_capture_error","create file");return;}
    const bool complete=std::fwrite(source,1,size,file)==size;
    const int closed=std::fclose(file);
    if(!complete || closed) {
        captureRoot[0]=0;twv::vita::telemetryText("shader_source_capture_error","write/close");return;
    }
    ++captured;capturedBytes+=size;
    twv::vita::telemetryEvent("shader_sources_captured",captured);
    twv::vita::telemetryEvent("shader_source_bytes_captured",capturedBytes);
#else
    (void)stage;(void)source;
#endif
}
void observe_shader_compile(uint64_t microseconds) {
    ++compiles;compileUs+=microseconds;
    if(!twv::vita::runControlPlayerSeen())return;
    ++playCompiles;playCompileUs+=microseconds;
    if(microseconds>playCompileMaxUs)playCompileMaxUs=microseconds;
    if(microseconds>=SlowCompileUs)++playSlowCompiles;
}
void shader_compile_report() {
    twv::vita::telemetryEvent("gfx_shader_native_compiles",compiles);
    twv::vita::telemetryEvent("gfx_shader_native_compile_us",compileUs);
    twv::vita::telemetryEvent("gfx_shader_play_compiles",playCompiles);
    twv::vita::telemetryEvent("gfx_shader_play_compile_us",playCompileUs);
    twv::vita::telemetryEvent("gfx_shader_play_compile_max_us",playCompileMaxUs);
    twv::vita::telemetryEvent("gfx_shader_play_slow_compiles",playSlowCompiles);
}
}

// --vita-shader-capture=on|off (main_vita.cpp, start-up only): record every generated stage the
// session compiles, for tools/vita_shader_warmup.py. Unavailable when the build compiled it out.
extern "C" int twv_vita_shader_capture_select(int enabled) {
#if TWV_VITA_SHADER_SOURCE_CAPTURE
    twv::vgl::captureSelected=enabled!=0;
    return 1;
#else
    return enabled==0;
#endif
}
