// Read-only diagnostic, built with the original game's int-enum ABI.
// Runs on the main thread after the FIFO drain, never while actors update.
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "d/actor/d_a_e_oc.h"
#include "d/actor/d_a_alink.h"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_ext.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "SSystem/SComponent/c_counter.h"
#include "telemetry.hpp"
#include "simulation_probe.hpp"
#include "target_probe.hpp"
#include "run_control.hpp"
#include "dusk/game_clock.h"
#include "dusk/settings.h"
#include <cstdio>

namespace {
struct ActorCounts { unsigned total=0,enemies=0,creating=0,samples=0; };
int observe_actor(void* raw,void* data) {
    auto& counts=*static_cast<ActorCounts*>(data);
    auto* actor=static_cast<fopAc_ac_c*>(raw);
    ++counts.total;
    if(fpcM_IsCreating(fopAcM_GetID(actor))) {++counts.creating;return 1;}
    if(actor->group!=fopAc_ENEMY_e)return 1;
    ++counts.enemies;
    if(counts.samples++>=8)return 1;
    char value[256];
    std::snprintf(value,sizeof value,"id=%lu profile=%d params=%08lx room=%d hp=%d pos=%.3f,%.3f,%.3f",
        static_cast<unsigned long>(fopAcM_GetID(actor)),int(fopAcM_GetProfName(actor)),
        static_cast<unsigned long>(fopAcM_GetParam(actor)),int(actor->current.roomNo),int(actor->health),
        double(actor->current.pos.x),double(actor->current.pos.y),double(actor->current.pos.z));
    twv::vita::telemetryText("enemy_actor",value);
    if(fopAcM_GetProfName(actor)==fpcNm_E_OC_e) {
        auto* oc=static_cast<daE_OC_c*>(actor); // getActionMode is a read-only, non-const upstream getter.
        std::snprintf(value,sizeof value,"id=%lu action=%d mode=%d timer=%d speed=%.3f",
            static_cast<unsigned long>(fopAcM_GetID(actor)),int(oc->getActionMode()),int(oc->mOcState),
            int(oc->field_0x6c0),double(oc->speedF));
        twv::vita::telemetryText("enemy_bokoblin_state",value);
    }
    return 1;
}
// The game's own heaps (JKR, carved from the fixed MEM1/ARAM reserves): "name=free/largest/size"
// bytes. A long-play leak inside them shows here and not in the malloc receipts (cpu_heap_*).
// Sampled at every probe call (every 10 frames with full receipts, 300 when quiet); "jkr_heaps" is
// the sample at the report, "jkr_heaps_low" the lowest free and largest block since the previous
// report (loads peak between reports). Read-only: each query takes the heap's lock and walks its free list.
constexpr unsigned HeapCount=7;
long heapLowFree[HeapCount],heapLowLargest[HeapCount];
bool heapLowValid=false;
void heap_receipts(bool report) {
    struct Heap {const char* name;JKRHeap* heap;};
    const Heap heaps[HeapCount]={{"root",JKRHeap::getRootHeap()},{"system",JKRHeap::getSystemHeap()},
        {"zelda",mDoExt_getZeldaHeap()},{"game",mDoExt_getGameHeap()},{"archive",mDoExt_getArchiveHeap()},
        {"j2d",mDoExt_getJ2dHeap()},{"command",mDoExt_getCommandHeap()}};
    char text[400],low[400];std::size_t used=0,lowUsed=0;
    for(unsigned i=0;i<HeapCount;++i) {
        const auto& h=heaps[i];
        if(!h.heap)continue;
        const long free=long(h.heap->getTotalFreeSize()),largest=long(h.heap->getFreeSize());
        if(!heapLowValid || free<heapLowFree[i])heapLowFree[i]=free;
        if(!heapLowValid || largest<heapLowLargest[i])heapLowLargest[i]=largest;
        if(!report)continue;
        if(used<sizeof text) {
            const int n=std::snprintf(text+used,sizeof text-used,"%s%s=%ld/%ld/%lu",used?";":"",h.name,free,largest,
                static_cast<unsigned long>(h.heap->getHeapSize()));
            if(n>0)used+=std::size_t(n);
        }
        if(lowUsed<sizeof low) {
            const int n=std::snprintf(low+lowUsed,sizeof low-lowUsed,"%s%s=%ld/%ld/%lu",lowUsed?";":"",h.name,
                heapLowFree[i],heapLowLargest[i],static_cast<unsigned long>(h.heap->getHeapSize()));
            if(n>0)lowUsed+=std::size_t(n);
        }
    }
    heapLowValid=!report;
    if(used)twv::vita::telemetryText("jkr_heaps",text);
    if(lowUsed)twv::vita::telemetryText("jkr_heaps_low",low);
    // Link's two own heaps (daAlink_c::setOriginalHeap: his clothes' archive and his shield's), each
    // "free now/lowest free since the start/size": what the archives really take of them.
    static long linkLow[2]={-1,-1};
    if(const daAlink_c* link=daAlink_getAlinkActorClass()) {
        JKRExpHeap* const own[2]={link->mpArcHeap,link->mpShieldArcHeap};
        char line[160];std::size_t at=0;
        for(unsigned i=0;i<2;++i) {
            if(!own[i])continue;
            const long free=long(own[i]->getTotalFreeSize());
            if(linkLow[i]<0 || free<linkLow[i])linkLow[i]=free;
            if(report && at<sizeof line) {
                const int n=std::snprintf(line+at,sizeof line-at,"%s%s=%ld/%ld/%lu",at?";":"",i?"shield":"arc",free,linkLow[i],
                    static_cast<unsigned long>(own[i]->getHeapSize()));
                if(n>0)at+=std::size_t(n);
            }
        }
        if(report && at)twv::vita::telemetryText("link_heaps",line);
    }
}
}

extern "C" void twv_vita_scene_probe(unsigned frame) {
    // Free play (telemetry.hpp, quiet): stage, player and game ticks every 300 frames only.
    const bool quiet = twv::vita::telemetryIsQuiet();
    if (!quiet) twv_vita_target_probe(frame);
    if (quiet ? frame % twv::vita::quietReceiptFrames() != 0 : (frame != 1 && frame % 10)) return;
    twv::vita::telemetryEvent("runtime_frames", frame);
    const char* stage = dComIfGp_getStartStageName();
    twv::vita::telemetryText("stage_name", stage ? stage : "");
    twv::vita::telemetryEvent("stage_room", dComIfGp_getStartStageRoomNo());
    twv::vita::telemetryEvent("stage_point", dComIfGp_getStartStagePoint());
    twv::vita::telemetryEvent("stage_layer", dComIfGp_getStartStageLayer());
    const auto* player = dComIfGp_getPlayer(0);
    twv::vita::telemetryEvent("player_exists", player != nullptr);
    twv_vita_run_player_seen(player != nullptr);
    if (player) {
        char value[192];
        std::snprintf(value, sizeof value, "pos=%.6f,%.6f,%.6f rot=%d,%d,%d room=%d",
                      double(player->current.pos.x), double(player->current.pos.y),
                      double(player->current.pos.z), int(player->shape_angle.x),
                      int(player->shape_angle.y), int(player->shape_angle.z), int(player->current.roomNo));
        twv::vita::telemetryText("player_state", value);
    }
    heap_receipts(frame % twv::vita::QuietReceiptFrames == 0); // reported about every 10 s, either mode
    if (quiet) {
        twv::vita::telemetryEvent("simulation_execute_calls",twv::vita::simulationExecuteCalls);
        twv::vita::telemetryEvent("presentation_interpolating",dusk::game_clock::g_frameTiming.interpolating);
        return;
    }
    char input[128];
    std::snprintf(input,sizeof input,"left=%.4f,%.4f right=%.4f,%.4f",
                  double(mDoCPd_c::getStickX(0)),double(mDoCPd_c::getStickY(0)),
                  double(mDoCPd_c::getSubStickX(0)),double(mDoCPd_c::getSubStickY(0)));
    twv::vita::telemetryText("game_pad",input);
    twv::vita::telemetryEvent("game_pad_buttons",mDoCPd_c::getHold(0));
    if(frame%30==0) {
        twv::vita::telemetryEvent("simulation_execute_calls",twv::vita::simulationExecuteCalls);
        twv::vita::telemetryEvent("simulation_rate_millihz",static_cast<long long>(
            dusk::game_clock::get_sim_rate()*1000.f+0.5f));
        twv::vita::telemetryEvent("presentation_interpolating",dusk::game_clock::g_frameTiming.interpolating);
        twv::vita::telemetryEvent("presentation_separate",dusk::game_clock::g_frameTiming.separatePresentation);
        twv::vita::telemetryEvent("presentation_cap_hz",dusk::getSettings().video.maxFrameRate.getValue());
        ActorCounts counts;
        fopAcIt_Executor(observe_actor,&counts);
        twv::vita::telemetryEvent("actor_count",counts.total);
        twv::vita::telemetryEvent("enemy_count",counts.enemies);
        twv::vita::telemetryEvent("actor_creating_count",counts.creating);
        twv::vita::telemetryEvent("player_life_units",dComIfGs_getLife());
        twv::vita::telemetryEvent("selected_sword",dComIfGs_getSelectEquipSword());
        twv::vita::telemetryEvent("selected_shield",dComIfGs_getSelectEquipShield());
        twv::vita::telemetryEvent("selected_clothes",dComIfGs_getSelectEquipClothes());
    }
    if (const auto* camera=dComIfGp_getCamera(0)) {
        const auto& view=camera->view;
        char value[256];
        std::snprintf(value,sizeof value,"eye=%.6f,%.6f,%.6f center=%.6f,%.6f,%.6f fov=%.6f",
          double(view.lookat.eye.x),double(view.lookat.eye.y),double(view.lookat.eye.z),
          double(view.lookat.center.x),double(view.lookat.center.y),double(view.lookat.center.z),double(view.fovy));
        twv::vita::telemetryText("camera_state",value);
    }
    twv::vita::telemetryFlush();
}

// Settings page (ui/settings_screen.cpp): the game at the moment the page opens and closes. Equal
// texts (ticks, game counters, Link, camera) show the page changed nothing while it was open.
extern "C" void twv_vita_scene_snapshot(const char* tag) {
    char value[320];
    int n = std::snprintf(value, sizeof value, "%s ticks=%llu counter=%lu timer=%lu", tag,
                          static_cast<unsigned long long>(twv::vita::simulationExecuteCalls),
                          static_cast<unsigned long>(g_Counter.mCounter0), static_cast<unsigned long>(g_Counter.mTimer));
    if (const auto* player = dComIfGp_getPlayer(0); player && n > 0 && n < int(sizeof value)) {
        n += std::snprintf(value + n, sizeof value - n, " pos=%.6f,%.6f,%.6f rot=%d,%d,%d speed=%.6f",
                           double(player->current.pos.x), double(player->current.pos.y), double(player->current.pos.z),
                           int(player->shape_angle.x), int(player->shape_angle.y), int(player->shape_angle.z),
                           double(player->speedF));
    }
    if (const auto* camera = dComIfGp_getCamera(0); camera && n > 0 && n < int(sizeof value)) {
        std::snprintf(value + n, sizeof value - n, " eye=%.6f,%.6f,%.6f", double(camera->view.lookat.eye.x),
                      double(camera->view.lookat.eye.y), double(camera->view.lookat.eye.z));
    }
    twv::vita::telemetryText("settings_snapshot", value);
}
