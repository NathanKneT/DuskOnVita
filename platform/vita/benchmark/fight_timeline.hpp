#pragma once
// Fixture only: a scripted 450-frame controller input for the opt-in benchmark
// (--vita-benchmark); no actor or game data.
#include <cstdint>
#include <cmath>
#include <algorithm>
namespace twv::benchmark {
struct Key {unsigned frame;float x,y,cx,cy;uint32_t buttons;float left,right;};
inline constexpr unsigned FightFrames=450;
inline constexpr Key FightKeys[]={
    {0,0.0f,0.0f,0.0f,0.0f,0x0,0.0f,0.0f},
    {15,0.0f,0.65f,0.0f,0.0f,0x40,1.0f,0.0f},
    {55,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {57,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {69,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {71,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {83,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {85,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {97,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {135,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {140,0.0f,0.0f,0.0f,0.0f,0x60,1.0f,1.0f},
    {175,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {185,0.7f,0.0f,0.0f,0.0f,0x140,1.0f,0.0f},
    {187,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {200,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {202,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {214,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {216,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {228,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {230,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {250,0.0f,0.0f,0.0f,0.0f,0x60,1.0f,1.0f},
    {290,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {310,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {312,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {324,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {326,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {340,0.0f,0.0f,0.0f,0.0f,0x240,1.0f,0.0f},
    {380,0.0f,0.0f,0.0f,0.0f,0x40,1.0f,0.0f},
    {400,0.0f,0.0f,0.0f,0.0f,0x0,0.0f,0.0f},
};
inline Key fight_key_at(unsigned frame) {
    Key key{};
    for(const auto& candidate:FightKeys) {if(candidate.frame>frame)break;key=candidate;}
    return key;
}
// Same inverse of original PADClamp as the existing parity fixture producer.
inline int8_t stick_raw(float v) {
    if(!std::isfinite(v) || v==0.f)return 0;
    const int r=int(std::lround(15.f+std::min(std::fabs(v),113.f/112.f)*112.f));
    return int8_t(v<0.f?-std::min(r,128):std::min(r,127));
}
inline uint8_t trigger_raw(float v) {
    return !std::isfinite(v) || v<=0.f?0:uint8_t(std::lround(30.f+std::min(v,1.f)*150.f));
}
}
