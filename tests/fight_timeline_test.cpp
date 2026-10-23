#include "benchmark/fight_timeline.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
int main() {
    using namespace twv::benchmark;
    unsigned prior=0;
    for(unsigned i=0;i<sizeof(FightKeys)/sizeof(FightKeys[0]);++i) {
        const auto& key=FightKeys[i];
        assert(i==0 || key.frame>prior);prior=key.frame;
        assert(key.frame<FightFrames && (key.buttons&~0x360u)==0);
    }
    assert(fight_key_at(0).buttons==0 && fight_key_at(14).y==0);
    assert(fight_key_at(15).buttons==0x40 && fight_key_at(54).y==0.65f);
    assert(fight_key_at(55).buttons==0x240 && fight_key_at(57).buttons==0x40);
    assert(fight_key_at(140).buttons==0x60 && fight_key_at(174).right==1.f);
    assert(fight_key_at(185).buttons==0x140 && fight_key_at(186).x==0.7f);
    for(unsigned frame=400;frame<=FightFrames;++frame)assert(fight_key_at(frame).buttons==0);
    assert(stick_raw(0)==0 && stick_raw(1)==127 && stick_raw(-1)==-127);
    assert(stick_raw(0.65f)==88 && stick_raw(-0.7f)==-93);
    assert(stick_raw(std::numeric_limits<float>::infinity())==0);
    assert(trigger_raw(0)==0 && trigger_raw(1)==180 && trigger_raw(2)==180);
    std::puts("fight_timeline_test: fixture key boundaries, neutral release, original PAD dead-zone conversion and trigger clamps passed");
}
