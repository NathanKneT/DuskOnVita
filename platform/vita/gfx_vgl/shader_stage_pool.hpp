#pragma once
// Serialized Cg shader objects only. Linked-program uniforms/attributes remain
// private to each program. No disk cache, GPU pool or shader source is changed.
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace twv::vgl {
template<size_t MaxEntries=128,size_t MaxSourceBytes=512*1024>
class ShaderStagePool {
    struct Entry {
        std::unique_ptr<char[]> source;
        size_t bytes=0;
        uint32_t stage=0,shader=0,references=0;
    };
    std::array<Entry,MaxEntries> entries{};
    size_t bytes_=0,live_=0;
public:
    struct Stats {uint64_t hits=0,fallbacks=0,peakSourceBytes=0;} stats;
    size_t source_bytes()const{return bytes_;}
    size_t live_entries()const{return live_;}

    template<class Compile>
    uint32_t acquire(uint32_t stage,const char* source,Compile compile) {
        const size_t bytes=std::strlen(source)+1;
        Entry* vacant=nullptr;
        for(auto& entry:entries) {
            if(!entry.shader) {if(!vacant)vacant=&entry;continue;}
            if(entry.stage==stage && entry.bytes==bytes &&
               std::memcmp(entry.source.get(),source,bytes)==0) {
                ++entry.references;++stats.hits;return entry.shader;
            }
        }
        // Capacity/allocation pressure uses the original uncached compilation.
        // Never evict a shader still referenced by another linked program.
        std::unique_ptr<char[]> copy;
        if(vacant && bytes<=MaxSourceBytes-bytes_)copy.reset(new(std::nothrow) char[bytes]);
        if(!copy) {++stats.fallbacks;return compile(stage,source);}
        std::memcpy(copy.get(),source,bytes);
        const uint32_t shader=compile(stage,source);
        if(!shader)return 0; // A failed compilation never becomes a cache hit.
        vacant->source=std::move(copy);vacant->bytes=bytes;
        vacant->stage=stage;vacant->shader=shader;vacant->references=1;
        bytes_+=bytes;++live_;
        if(bytes_>stats.peakSourceBytes)stats.peakSourceBytes=bytes_;
        return shader;
    }

    template<class Destroy>
    void release(uint32_t shader,Destroy destroy) {
        if(!shader)return;
        for(auto& entry:entries)if(entry.shader==shader) {
            if(--entry.references)return;
            // Caller has already detached this program. The compiled stage is
            // retained until the last independent program owner releases it.
            destroy(shader);bytes_-=entry.bytes;--live_;entry=Entry{};return;
        }
        destroy(shader); // Uncached fallback still owns its native object.
    }
};
}
