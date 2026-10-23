#pragma once
#include <cstdint>

namespace twv::vgl {
// Preserve ascending original light order and independent RGB/alpha masks.
// Zero-mask lights contribute nothing; deleting their shader work is exact.
struct LightPlan {
    unsigned count=0;
    unsigned indices[8]{};
    uint32_t masks[4]{};
};
template<class Lights>
void remove_zero_color_lights(uint32_t* masks,const Lights& lights) {
    for(unsigned i=0;i<8;++i) {
        const auto& color=lights[i].color;
        const bool rgb=color[0]!=0.f || color[1]!=0.f || color[2]!=0.f;
        const bool alpha=color[3]!=0.f;
        // RGB and alpha have independent GX channels. Black RGB with alpha1
        // must remain available to a lit alpha channel, and vice versa.
        if(!rgb){masks[0]&=~(1u<<i);masks[1]&=~(1u<<i);}
        if(!alpha){masks[2]&=~(1u<<i);masks[3]&=~(1u<<i);}
    }
}
inline LightPlan compact_light_plan(const uint32_t* masks,unsigned enabledChannels) {
    LightPlan plan;
    uint32_t used=0;
    for(unsigned ch=0;ch<4;++ch)if(enabledChannels&(1u<<ch))used|=masks[ch];
    for(unsigned light=0;light<8;++light)if(used&(1u<<light)) {
        const unsigned slot=plan.count++;
        plan.indices[slot]=light;
        for(unsigned ch=0;ch<4;++ch)
            if((enabledChannels&(1u<<ch)) && (masks[ch]&(1u<<light)))plan.masks[ch]|=1u<<slot;
    }
    return plan;
}
}
