#pragma once
#include "bundle_options.h"

namespace twv::vgl {
class BundleControl {
    const bool available;
    bool selected=false,started=false;
    uint32_t features=0,profile=0;
public:
    explicit BundleControl(bool compiled):available(compiled){}
    bool select(uint32_t mask,uint32_t memoryProfile) noexcept {
        if(selected || started || (mask&~TWV_BUNDLE_ALL) || memoryProfile>2 ||
           (!available && (mask || memoryProfile)))return false;
        features=mask;profile=memoryProfile;selected=true;return true;
    }
    bool enabled(uint32_t feature) noexcept {
        started=true;
        return feature && !(feature&(feature-1)) && !(feature&~TWV_BUNDLE_ALL) &&
               available && (features&feature);
    }
    uint32_t memory_profile() noexcept {started=true;return available?profile:0;}
    uint32_t mask() noexcept {started=true;return available?features:0;}
    uint32_t compiled()const noexcept{return available?1:0;}
};
}
