#pragma once
// Generated Cg source only. Native GXP ownership/serialization stays in the backend
// (vitaGL's cache for --vita-gpu=vgl, gfx_gxm's GXP cache for --vita-gpu=gxm).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace twv::vgl {
// A whole session's stages: V61 compiled 51-95 per scene, distinct across scenes; the whole-game
// tour found more than the bench scenes' 1024.
constexpr size_t ShaderSourceLimit=8*1024*1024,ShaderStageLimit=2048;
constexpr size_t ShaderBundleLimit=24+ShaderSourceLimit+ShaderStageLimit*9;
constexpr uint32_t VertexSourceTag=1,FragmentSourceTag=2;
inline uint32_t source_crc32(std::span<const uint8_t> bytes) {
    uint32_t crc=~uint32_t{0};
    for(auto byte:bytes) {
        crc^=byte;
        for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&uint32_t(0-(crc&1)));
    }
    return ~crc;
}
struct ShaderSourceView {uint32_t stage=0,bytes=0;const char* source=nullptr;};
struct ShaderBundleView {std::array<ShaderSourceView,ShaderStageLimit> sources{};uint32_t count=0;};
inline bool parse_shader_bundle(std::span<const uint8_t> bytes,ShaderBundleView& out) {
    out={};
    if(bytes.size()<24 || bytes.size()>ShaderBundleLimit ||
       std::memcmp(bytes.data(),"TWVCGB01",8))return false;
    const auto read=[&](size_t at) {
        return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|
            (uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);
    };
    const auto count=read(12);
    if(read(8)!=1 || !count || count>ShaderStageLimit || read(16)!=bytes.size()-24 ||
       read(20)!=source_crc32(bytes.subspan(24)))return false;
    size_t cursor=24,total=0;
    for(uint32_t i=0;i<count;++i) {
        if(bytes.size()-cursor<9)return false;
        const auto stage=read(cursor),size=read(cursor+4);cursor+=8;
        if((stage!=VertexSourceTag && stage!=FragmentSourceTag) || !size || size>65536 ||
           size>ShaderSourceLimit-total || size>=bytes.size()-cursor ||
           bytes[cursor+size] || std::memchr(bytes.data()+cursor,0,size))return false;
        out.sources[i]={stage,size,reinterpret_cast<const char*>(bytes.data()+cursor)};
        total+=size;cursor+=size+1;
    }
    if(cursor!=bytes.size())return false;
    out.count=count;return true;
}
using PrepareCompile=uint32_t(*)(uint32_t stage,const char* source);
using PrepareRelease=void(*)(uint32_t shader);
// After each stage of a valid bundle: stages handled so far and the bundle's stage count.
using PrepareProgress=void(*)(uint32_t done,uint32_t total);
// SDK-domain file operations; called once before the first original game frame.
void prepare_shader_cache(const char* path,PrepareCompile compile,PrepareRelease release,PrepareProgress progress=nullptr);
// The bundle to prepare: ux0:data/twilight-vita/shader_warmup.bin; when it is missing and the build
// packages one (TWV_VITA_APP_WARMUP_BUNDLE), app0:shader_warmup.bin. Receipt shader_warmup_source.
constexpr const char* DataShaderBundle="ux0:data/twilight-vita/shader_warmup.bin";
constexpr const char* AppShaderBundle="app0:shader_warmup.bin";
const char* shader_bundle_path();
// Recording happens only when --vita-shader-capture=on chose it (and the build compiled it in).
void begin_shader_source_capture(const char* root);
void capture_shader_source(uint32_t stage,const char* source);
// Every native stage compile (vitaGL loads cached GXP or runs the Cg compiler), timed. Those after
// Link first appears are play-time compiles: the stalls the warm-up bundle exists to remove.
void observe_shader_compile(uint64_t microseconds);
void shader_compile_report();
}
