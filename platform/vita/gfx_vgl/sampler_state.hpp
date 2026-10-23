#pragma once
#include <array>
#include <cstdint>

namespace twv::vgl {
struct SamplerParams {
    int min,mag,s,t;
    bool operator==(const SamplerParams& other) const {
        return min==other.min && mag==other.mag && s==other.s && t==other.t;
    }
};
// Codes mirror the bridge's existing GL conversions. Equivalent raw values
// share a key; this is deliberately limited to current min/mag/wrap support.
inline SamplerParams canonical_sampler_params(int min,int mag,int s,int t) noexcept {
    return {min>=0 && min<6?min:1,mag?1:0,s==1?1:s==2?2:0,t==1?1:t==2?2:0};
}
// Metadata only, owned per logical GX texture unit. Texture ID/deletion/upload
// cannot invalidate an owned sampler object's independent parameter values.
class UnitSamplerCache {
    std::array<SamplerParams,8> params_{};
    uint32_t valid_=0;
public:
    static constexpr unsigned Capacity=8;
    bool parameters(uint32_t unit,SamplerParams& out) const noexcept {
        if(unit>=Capacity || !(valid_&(1u<<unit)))return false;
        out=params_[unit];return true;
    }
    bool unchanged(uint32_t unit,SamplerParams params) const noexcept {
        SamplerParams current{};return parameters(unit,current) && current==params;
    }
    void applied(uint32_t unit,SamplerParams params) noexcept {
        if(unit<Capacity){params_[unit]=params;valid_|=1u<<unit;}
    }
    void invalidate(uint32_t unit) noexcept {if(unit<Capacity)valid_&=~(1u<<unit);}
    void clear() noexcept {valid_=0;}
};
// Fixed-size shadow of parameters actually applied to texture objects. This is
// not a binding cache: draws still explicitly bind every sampled texture unit.
// A slot collision causes another real update, never a false hit or allocation.
class TextureSamplerCache {
    struct Entry {uint32_t texture=0;SamplerParams params{};};
    std::array<Entry,256> entries{};
public:
    static constexpr unsigned Capacity=256;
    bool unchanged(uint32_t texture,SamplerParams params) const {
        const auto& e=entries[texture%Capacity];
        return texture && e.texture==texture && e.params==params;
    }
    void applied(uint32_t texture,SamplerParams params) {
        if(texture)entries[texture%Capacity]={texture,params};
    }
    void invalidate(uint32_t texture) {
        auto& e=entries[texture%Capacity];
        if(e.texture==texture)e={};
    }
    void clear(){entries={};}
};
}
