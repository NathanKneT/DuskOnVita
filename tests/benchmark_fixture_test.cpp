// Link the actual production fixture.cpp with minimal read-only game stubs.
// Drive simulation ticks with0/1/2/3 presentation frames between them, and with
// multiple catch-up ticks per presentation. Compare every original PAD sample.
#include "benchmark/fight_timeline.hpp"
#include "runtime/telemetry.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
struct Receipt {std::string key;long long value;};
std::vector<Receipt> receipts;
unsigned failures=0,itemCalls=0;float selectedTime=0;
namespace twv::vita {
void telemetryEvent(const char* key,long long value){receipts.push_back({key,value});}
void telemetryText(const char* key,const char*){failures+=std::string(key)=="benchmark_failure";}
void telemetryFlush(){}
}
void item_func_SWORD(){++itemCalls;}
void item_func_WOOD_SHIELD(){++itemCalls;}
void item_func_WEAR_KOKIRI(){++itemCalls;}
void dComIfGs_setTime(float t){selectedTime=t;}
extern "C" int twv_vita_benchmark_enable(const char*);
extern "C" void twv_vita_benchmark_blank_save();
extern "C" void twv_vita_benchmark_pad(PADStatus*);
extern "C" void twv_vita_benchmark_tick();
int main(int argc,char** argv){
    assert(argc==2);const std::string mode=argv[1];
    PADStatus pads[4]{};
    if(mode=="off"){
        pads[0].button=0x1234;twv_vita_benchmark_pad(pads);twv_vita_benchmark_tick();
        assert(pads[0].button==0x1234 && receipts.empty() && !itemCalls);
    }else{
        assert(!twv_vita_benchmark_enable("unknown"));
        assert(twv_vita_benchmark_enable("C1-FIGHT"));assert(!twv_vita_benchmark_enable("C1-FIGHT"));
        if(mode=="wrong-stage")dusk::StageRequested.room=1;
        twv_vita_benchmark_blank_save();
        if(mode=="wrong-stage"){assert(failures==1 && !itemCalls);return 0;}
        assert(itemCalls==3 && selectedTime==180.f);
        for(unsigned i=0;i<30;++i){
            twv_vita_benchmark_pad(pads);assert(!pads[0].button);
            twv_vita_benchmark_tick();
        }
        assert(!receipts.empty() && receipts.back().key=="benchmark_ready");
        unsigned tick=0,presentations=0;
        while(tick<twv::benchmark::FightFrames){
            const unsigned catchup=mode=="catchup"?1+(presentations%3):1;
            for(unsigned i=0;i<catchup && tick<twv::benchmark::FightFrames;++i,++tick){
                const auto expected=twv::benchmark::fight_key_at(tick);
                twv_vita_benchmark_pad(pads);
                assert(pads[0].button==expected.buttons && pads[0].err==PAD_ERR_NONE);
                assert(pads[0].stickX==twv::benchmark::stick_raw(expected.x));
                assert(pads[0].stickY==twv::benchmark::stick_raw(expected.y));
                assert(pads[0].substickX==twv::benchmark::stick_raw(expected.cx));
                assert(pads[0].substickY==twv::benchmark::stick_raw(expected.cy));
                for(unsigned p=1;p<4;++p)assert(pads[p].err==PAD_ERR_NO_CONTROLLER);
                twv_vita_benchmark_tick();
            }
            // Presentation-only work has no call into the fixture. Deliberately
            // vary its frequency; the next PAD expectation must still advance1.
            presentations+=mode=="60hz"?2:mode=="variable"?presentations%3+1:1;
        }
        unsigned done=0,logged=0;
        for(const auto& r:receipts){
            if(r.key=="benchmark_tick"){assert(r.value==(logged<45?10*logged:449));++logged;}
            if(r.key=="benchmark_done"){assert(r.value==450);++done;}
        }
        assert(logged==46 && done==1 && !failures);
        for(unsigned i=0;i<10;++i){twv_vita_benchmark_pad(pads);twv_vita_benchmark_tick();assert(!pads[0].button);}
    }
    std::printf("benchmark_fixture_test: %s actual fixture passed\n",mode.c_str());
}
