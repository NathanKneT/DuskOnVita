#include "gfx_vgl/light_plan.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    using twv::vgl::compact_light_plan;
    // Every single light including slot7, no lights, and all eight. RGB and
    // alpha can use disjoint masks; a non-rendered channel must not add lights.
    uint32_t masks[4]={0x81,0x40,0x02,0xFF};
    auto plan=compact_light_plan(masks,5);
    assert(plan.count==3 && plan.indices[0]==0 && plan.indices[1]==1 && plan.indices[2]==7);
    assert(plan.masks[0]==5 && plan.masks[2]==2 && plan.masks[1]==0 && plan.masks[3]==0);
    assert(compact_light_plan(masks,0).count==0);
    assert(compact_light_plan(masks,15).count==8);
    struct Light {std::array<float,4> color;};
    std::array<Light,8> lights{};
    lights[0].color={0,0,0,1};lights[2].color={1,0.5f,0,0};
    uint32_t zeroMasks[4]={255,255,255,255};
    twv::vgl::remove_zero_color_lights(zeroMasks,lights);
    assert(zeroMasks[0]==4 && zeroMasks[1]==4 && zeroMasks[2]==1 && zeroMasks[3]==1);
    lights[7].color[0]=std::numeric_limits<float>::quiet_NaN();
    uint32_t nonfinite[4]={128,128,128,128};
    twv::vgl::remove_zero_color_lights(nonfinite,lights);
    assert(nonfinite[0]==128 && nonfinite[2]==0); // Do not silently classify NaN as black.
    uint32_t seed=0x94752;
    for(unsigned sample=0;sample<65536;++sample) {
        const auto next=[&](){seed=seed*1664525u+1013904223u;return seed;};
        for(auto& mask:masks)mask=(next()>>16)&255;
        const unsigned enabled=(next()>>16)&15;
        plan=compact_light_plan(masks,enabled);
        assert(plan.count<=8);
        for(unsigned i=1;i<plan.count;++i)assert(plan.indices[i]>plan.indices[i-1]);
        // Order-sensitive float accumulation must agree bit-for-bit, not just
        // counts. These stand in for each original light's computed factor.
        const float contribution[8]={1e5f,-1e5f,0.0007f,71.2f,-0.2f,0.1234f,18.2f,-7.f};
        for(unsigned ch=0;ch<4;++ch) {
            float original=0.125f,compact=original;
            if(enabled&(1u<<ch))for(unsigned i=0;i<8;++i)
                if(masks[ch]&(1u<<i))original+=contribution[i];
            for(unsigned i=0;i<plan.count;++i)if(plan.masks[ch]&(1u<<i))compact+=contribution[plan.indices[i]];
            assert(original==compact);
        }
        for(auto& light:lights)for(auto& value:light.color)value=float(int((next()>>16)%5)-2)*0.25f;
        uint32_t filtered[4];for(unsigned ch=0;ch<4;++ch)filtered[ch]=masks[ch];
        twv::vgl::remove_zero_color_lights(filtered,lights);
        plan=compact_light_plan(filtered,enabled);
        for(unsigned ch=0;ch<4;++ch)for(unsigned component=0;component<(ch<2?3u:1u);++component) {
            const unsigned colorComponent=ch<2?component:3;
            float original=0.125f,compact=original;
            if(enabled&(1u<<ch))for(unsigned i=0;i<8;++i)
                if(masks[ch]&(1u<<i))original+=contribution[i]*lights[i].color[colorComponent];
            for(unsigned i=0;i<plan.count;++i)if(plan.masks[ch]&(1u<<i))
                compact+=contribution[plan.indices[i]]*lights[plan.indices[i]].color[colorComponent];
            assert(original==compact);
        }
    }
    std::puts("light_plan_test: 65536 independent RGB/alpha masks, live slot7, empty/full plans and exact accumulation order passed");
}
