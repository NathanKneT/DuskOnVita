#include "gfx_vgl/shader_preparation.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

std::map<std::string,long long> events;
std::vector<std::pair<std::string,std::string>> texts;
namespace twv::vita {
void telemetryEvent(const char* key,long long value){events[key]=value;}
void telemetryText(const char* key,const char* text){texts.emplace_back(key,text);}
void telemetryFlush(){}
uint64_t processTimeUs(){return 123;} // Repeat stamp: capture must create a new directory.
bool playerSeen=false;
bool runControlPlayerSeen(){return playerSeen;}
}
extern "C" int twv_vita_shader_capture_select(int enabled);
unsigned calls=0,releases=0,live=0,peak=0;
bool failFirst=false;
uint32_t compile(uint32_t stage,const char* source) {
    assert((stage==1 || stage==2) && source && *source);++calls;
    if(failFirst){failFirst=false;return 0;}
    ++live;peak=std::max(peak,live);return calls;
}
void release(uint32_t id){assert(id && live==1);--live;++releases;}
using Bytes=std::vector<uint8_t>;
Bytes read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);assert(file);
    return Bytes(std::istreambuf_iterator<char>(file),{});
}
void write(const std::filesystem::path& path,const Bytes& bytes) {
    std::ofstream file(path,std::ios::binary);assert(file);
    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();assert(file);
}
void put(Bytes& bytes,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(at+i)=uint8_t(value>>(8*i));
}
void seal(Bytes& bytes) {
    put(bytes,16,bytes.size()-24);
    put(bytes,20,twv::vgl::source_crc32(std::span(bytes).subspan(24)));
}
int main(int argc,char** argv) {
    assert(argc==3);using namespace twv::vgl;
    const Bytes valid=read(argv[1]);const auto directory=std::filesystem::path(argv[2]);
    std::filesystem::create_directories(directory);
    assert(source_crc32({reinterpret_cast<const uint8_t*>("123456789"),9})==0xcbf43926);
    ShaderBundleView view;
    assert(parse_shader_bundle(valid,view) && view.count==2);
    assert(view.sources[0].stage==1 && std::string(view.sources[0].source)=="vertex_source");
    assert(view.sources[1].stage==2 && std::string(view.sources[1].source)=="fragment_source");
    for(size_t cut=0;cut<valid.size();++cut)assert(!parse_shader_bundle(std::span(valid).first(cut),view));
    for(size_t i=0;i<valid.size();++i) {
        auto corrupt=valid;corrupt[i]^=1;assert(!parse_shader_bundle(corrupt,view));
    }
    std::vector<Bytes> invalid;
    for(auto [offset,value]:std::vector<std::pair<size_t,uint32_t>>{{8,2},{12,0},{12,uint32_t(ShaderStageLimit+1)},{24,0},{24,3},{28,0},{28,65537},{28,UINT32_MAX}}) {
        auto bytes=valid;put(bytes,offset,value);seal(bytes);invalid.push_back(bytes);
        assert(!parse_shader_bundle(bytes,view));
    }
    auto trailing=valid;trailing.push_back(7);seal(trailing);invalid.push_back(trailing);
    assert(!parse_shader_bundle(trailing,view));
    auto nul=valid;nul[32]=0;seal(nul);invalid.push_back(nul);assert(!parse_shader_bundle(nul,view));
    uint32_t rng=0x451234;
    for(unsigned i=0;i<10000;++i) {
        rng=rng*1664525u+1013904223u;Bytes random(rng%256);
        for(auto& byte:random){rng=rng*1664525u+1013904223u;byte=rng>>24;}
        assert(!parse_shader_bundle(random,view));
    }
    const auto input=directory/"warmup.bin";write(input,valid);
    prepare_shader_cache(input.c_str(),compile,release);
#if TWV_VITA_SHADER_WARMUP
    assert(calls==2 && releases==2 && !live && peak==1);
    assert(events.at("shader_warmup_prepared")==2 && !events.at("shader_warmup_failed"));
    failFirst=true;prepare_shader_cache(input.c_str(),compile,release);
    assert(calls==4 && releases==3 && !live && events.at("shader_warmup_failed")==1);
#else
    assert(!calls && !releases && events.empty() && texts.empty());
#endif
    const auto before=calls;
    for(const auto& bytes:invalid) {write(input,bytes);prepare_shader_cache(input.c_str(),compile,release);assert(calls==before);}
    Bytes oversized(ShaderBundleLimit+1);write(input,oversized);
    prepare_shader_cache(input.c_str(),compile,release);assert(calls==before);
    prepare_shader_cache((directory/"absent").c_str(),compile,release);assert(calls==before);
    const auto capture=directory/"capture";
    // Not selected at start-up: nothing is recorded, in either build.
    begin_shader_source_capture(capture.c_str());capture_shader_source(1,"vertex_source");
    assert(!std::filesystem::exists(capture));
    assert(twv_vita_shader_capture_select(1)==TWV_VITA_SHADER_SOURCE_CAPTURE && twv_vita_shader_capture_select(0)==1);
    assert(twv_vita_shader_capture_select(1)==TWV_VITA_SHADER_SOURCE_CAPTURE);
    begin_shader_source_capture(capture.c_str());capture_shader_source(1,"vertex_source");capture_shader_source(2,"fragment_source");
#if TWV_VITA_SHADER_SOURCE_CAPTURE
    const auto first=capture/"run-000000000000007B-00";
    const auto firstFile=first/"0000-v.cg";const auto original=read(firstFile);
    assert(std::string(original.begin(),original.end())=="vertex_source");
    assert(read(first/"0001-f.cg").size()==15);
    begin_shader_source_capture(capture.c_str());
    const auto second=capture/"run-000000000000007B-01";
    for(unsigned i=0;i<=ShaderStageLimit;++i)capture_shader_source(1,"v");
    assert(read(firstFile)==original && !std::filesystem::exists(second/"1024-v.cg"));
    assert(std::distance(std::filesystem::directory_iterator(second),std::filesystem::directory_iterator{})==long(ShaderStageLimit));
    begin_shader_source_capture(capture.c_str());
    std::string huge(65537,'x');capture_shader_source(1,huge.c_str());
    assert(!std::filesystem::exists(capture/"run-000000000000007B-02/0000-v.cg"));
#else
    assert(!std::filesystem::exists(capture));
#endif
    // Compile timing: only compiles after the player appeared count as play-time stalls.
    events.clear();
    observe_shader_compile(9000);
    twv::vita::playerSeen=true;
    observe_shader_compile(12000);observe_shader_compile(400000);
    shader_compile_report();
    assert(events.at("gfx_shader_native_compiles")==3 && events.at("gfx_shader_native_compile_us")==421000);
    assert(events.at("gfx_shader_play_compiles")==2 && events.at("gfx_shader_play_compile_us")==412000);
    assert(events.at("gfx_shader_play_compile_max_us")==400000 && events.at("gfx_shader_play_slow_compiles")==1);
    std::printf("shader_preparation_test: warmup%d capture%d, Python bundle interoperability, all-before-native validation, bounds/fuzz, failure cleanup, one live shader and exclusive capture paths passed\n",TWV_VITA_SHADER_WARMUP,TWV_VITA_SHADER_SOURCE_CAPTURE);
}
