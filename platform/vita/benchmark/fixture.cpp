// Opt-in isolated test fixture. Never writes save files, actors, cameras or RNG.
// Only the declared blank-save equipment/time and original PAD inputs change.
#include "fight_timeline.hpp"
#include "runtime/telemetry.hpp"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "dusk/main.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_manager.h"
#include "m_Do/m_Do_graphic.h"
#include <cstdio>
#include <cstring>

namespace {
enum class Phase {Off,Waiting,Run,Done,Failed};
Phase phase=Phase::Off;
bool initialized=false,consumed=false;
unsigned tick=0,settled=0,waitTicks=0;
bool lastHit[3]{};uintptr_t lastTarget[3]{};
static_assert(PAD_BUTTON_A==0x100 && PAD_BUTTON_B==0x200 && PAD_TRIGGER_L==0x40 && PAD_TRIGGER_R==0x20);
daAlink_c* link_actor() {
    auto* actor=dComIfGp_getPlayer(0);
    if(!actor || fopAcM_GetProfName(actor)!=fpcNm_ALINK_e || fpcM_IsCreating(fopAcM_GetID(actor)))return nullptr;
    return static_cast<daAlink_c*>(actor);
}
bool play_tick() {return link_actor()!=nullptr && !fopOvlpM_IsDoingReq();}
bool ready() {
    const auto* link=link_actor();
    if(!link || fopOvlpM_IsDoingReq() || dComIfGp_event_runCheck() || link->mProcID!=daAlink_c::PROC_WAIT)return false;
    if(std::strcmp(dComIfGp_getStartStageName(),"F_SP108") || dComIfGp_roomControl_getStayNo()!=6)return false;
    const auto* fader=mDoGph_gInf_c::getFader();
    return !fader || fader->getStatus()==JUTFader::Wait;
}
void fail(const char* reason) {
    phase=Phase::Failed;twv::vita::telemetryText("benchmark_failure",reason);twv::vita::telemetryFlush();
}
}
extern "C" int twv_vita_benchmark_enable(const char* name) {
    if(std::strcmp(name,"C1-FIGHT") || phase!=Phase::Off)return 0;
    phase=Phase::Waiting;
    twv::vita::telemetryText("benchmark_requested",name);
    return 1;
}
extern "C" void twv_vita_benchmark_blank_save() {
    if(phase==Phase::Off)return;
    const auto& start=dusk::StageRequested;
    if(initialized || !start.set || start.stage!="F_SP108" || start.room!=6 || start.point!=1 || start.layer!=0) {
        fail("C1-FIGHT requires a fresh --stage F_SP108,6,1,0 start, no loaded save");return;
    }
    // The original game's own item-get calls. Initialize
    // before Link/model creation; no late actor manipulation or forced hits.
    item_func_SWORD();item_func_WOOD_SHIELD();item_func_WEAR_KOKIRI();
    dComIfGs_setTime(180.f);
    initialized=true;
    twv::vita::telemetryText("benchmark_fixture","C1-FIGHT; blank RAM save; Ordon sword,wood shield,hero clothes,time12; original spawn/camera/AI");
}
extern "C" void twv_vita_benchmark_pad(PADStatus* status) {
    if(phase==Phase::Off)return;
    consumed=phase==Phase::Run && play_tick();
    const auto key=consumed?twv::benchmark::fight_key_at(tick):twv::benchmark::Key{};
    auto& p=status[0];p={};p.err=PAD_ERR_NONE;
    p.button=key.buttons;
    p.stickX=twv::benchmark::stick_raw(key.x);p.stickY=twv::benchmark::stick_raw(key.y);
    p.substickX=twv::benchmark::stick_raw(key.cx);p.substickY=twv::benchmark::stick_raw(key.cy);
    p.triggerLeft=twv::benchmark::trigger_raw(key.left);p.triggerRight=twv::benchmark::trigger_raw(key.right);
    for(unsigned i=1;i<PAD_MAX_CONTROLLERS;++i){status[i]={};status[i].err=PAD_ERR_NO_CONTROLLER;}
}
// Called after each original game_clock::commit_sim_tick, including every
// catch-up tick. Presentation-only frames never advance the input timeline.
extern "C" void twv_vita_benchmark_tick() {
    if(phase==Phase::Off || phase==Phase::Done || phase==Phase::Failed)return;
    if(phase==Phase::Waiting) {
        if(++waitTicks>3600){fail("fixture readiness timed out");return;}
        if(!initialized || !ready()){settled=0;return;}
        if(++settled==30) {
            phase=Phase::Run;tick=0;
            twv::vita::telemetryEvent("benchmark_ready",0);twv::vita::telemetryFlush();
        }
        return;
    }
    if(!consumed)return;
    consumed=false;
    if(tick%10==0 || tick+1==twv::benchmark::FightFrames) {
        twv::vita::telemetryEvent("benchmark_tick",tick);
        twv::vita::telemetryEvent("benchmark_player_life",dComIfGs_getLife());
        if(const auto* link=link_actor())twv::vita::telemetryEvent("benchmark_player_proc",link->mProcID);
    }
    if(auto* link=link_actor())for(unsigned i=0;i<3;++i) {
        auto& capsule=link->mAtCps[i];
        const bool hit=capsule.ChkAtHit();
        const uintptr_t target=reinterpret_cast<uintptr_t>(capsule.GetAtHitAc());
        if(hit && (!lastHit[i] || target!=lastTarget[i])) {
            char value[128];
            std::snprintf(value,sizeof value,"tick=%u capsule=%u target_address=%lx shield=%d",tick,i,
                static_cast<unsigned long>(target),int(bool(capsule.ChkAtShieldHit())));
            // Targets can outlive a contact in the original capsule. Record
            // pointer identity only; never dereference a possibly stale target.
            twv::vita::telemetryText("benchmark_sword_contact_edge",value);
        }
        lastHit[i]=hit;lastTarget[i]=target;
    }
    if(++tick==twv::benchmark::FightFrames) {
        phase=Phase::Done;twv::vita::telemetryEvent("benchmark_done",tick);twv::vita::telemetryFlush();
    }
}
