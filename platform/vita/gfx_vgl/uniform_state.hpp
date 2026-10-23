#pragma once
// SDK-free shadows of this bridge's vec4 uniforms. A miss/eviction only causes
// another driver update. Shader uniforms belong to program objects, not draws.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace twv::vgl {
inline constexpr std::array<unsigned,15> UniformCapacity{4,4,4,2,30,30,8,8,8,8,8,4,4,8,48};
inline constexpr auto UniformOffset=[] {
    std::array<unsigned,UniformCapacity.size()> offsets{};
    for(unsigned i=1;i<offsets.size();++i)offsets[i]=offsets[i-1]+UniformCapacity[i-1];
    return offsets;
}();
inline constexpr unsigned UniformVectors=UniformOffset.back()+UniformCapacity.back();
static_assert(UniformVectors==178);

struct ProgramUniforms {
    uint32_t program=0;
    std::array<int32_t,UniformCapacity.size()> locations{};
    std::array<uint8_t,UniformCapacity.size()> valid{};
    std::array<float,UniformVectors*4> values{};

    bool unchanged(unsigned field,int32_t location,unsigned count,const float* data) const noexcept {
        return field<valid.size() && count && count<=valid[field] && locations[field]==location &&
            std::memcmp(values.data()+UniformOffset[field]*4,data,size_t(count)*16)==0;
    }
    void applied(unsigned field,int32_t location,unsigned count,const float* data) noexcept {
        if(field>=valid.size())return;
        if(!count || count>UniformCapacity[field]) {valid[field]=0;return;}
        if(locations[field]!=location)valid[field]=0;
        locations[field]=location;
        std::memcpy(values.data()+UniformOffset[field]*4,data,size_t(count)*16);
        // A shorter upload modifies only the prefix; retain the known tail.
        valid[field]=uint8_t(std::max(unsigned(valid[field]),count));
    }
};

class ProgramUniformCache {
public:
    static constexpr unsigned Ways=4,Sets=16,Entries=Ways*Sets;
    ProgramUniforms* select(uint32_t program) noexcept {
        if(!program)return nullptr;
        const auto set=program%Sets;
        auto* first=entries.data()+set*Ways;
        for(unsigned i=0;i<Ways;++i)if(first[i].program==program)return first+i;
        for(unsigned i=0;i<Ways;++i)if(!first[i].program){first[i]={};first[i].program=program;return first+i;}
        auto* victim=first+(next[set]++%Ways);
        *victim={};victim->program=program;
        return victim;
    }
    void invalidate(uint32_t program) noexcept {
        auto* first=entries.data()+(program%Sets)*Ways;
        for(unsigned i=0;i<Ways;++i)if(first[i].program==program)first[i].program=0;
    }
    void clear() noexcept {
        // No need to zero payloads until a slot is assigned to a new object.
        for(auto& entry:entries)entry.program=0;
        next.fill(0);
    }
private:
    std::array<ProgramUniforms,Entries> entries{};
    std::array<uint8_t,Sets> next{};
};
static_assert(sizeof(ProgramUniformCache)<192*1024);
} // namespace twv::vgl
