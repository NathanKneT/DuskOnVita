// Read-only diagnostic, built with the original game's int-enum ABI.
// Runs on the main thread after the FIFO drain, never while actors update.
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "d/actor/d_a_e_oc.h"
#include "f_op/f_op_camera_mng.h"
#include "m_Do/m_Do_controller_pad.h"
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
}

extern "C" void twv_vita_scene_probe(unsigned frame) {
    // Free play (telemetry.hpp, quiet): stage, player and game ticks every 300 frames only.
    const bool quiet = twv::vita::telemetryIsQuiet();
    if (!quiet) twv_vita_target_probe(frame);
    if (quiet ? frame % twv::vita::QuietReceiptFrames != 0 : (frame != 1 && frame % 10)) return;
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
