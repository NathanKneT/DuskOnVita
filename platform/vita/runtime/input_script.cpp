// Test-only scripted pad input (runtime/input_script.hpp), on patches/dusklight/0010's pad hook,
// which the opt-in benchmark fixture uses in benchmark builds (CMake keeps the two apart).
// Built with the game's int-enum ABI (dolphin PADStatus). Inactive unless main_vita.cpp loaded a
// script; once the script ends, the real pad is read untouched again.
#include "input_script.hpp"
#include "telemetry.hpp"
#include "d/d_com_inf_game.h"
#include <dolphin/pad.h>
#include <cstdio>

// ui/settings_screen.cpp (graphics builds): open the settings page at the next pass, or put two
// still fingers on the rear pad for its gesture recogniser.
extern "C" void twv_vita_settings_request_open(void) __attribute__((weak));
extern "C" void twv_vita_settings_inject_rear_hold(void) __attribute__((weak));

namespace {
static_assert(PAD_BUTTON_A==twv::vita::ScriptButtonA && PAD_BUTTON_B==twv::vita::ScriptButtonB &&
              PAD_BUTTON_X==twv::vita::ScriptButtonX && PAD_BUTTON_Y==twv::vita::ScriptButtonY &&
              PAD_BUTTON_START==twv::vita::ScriptButtonStart && PAD_TRIGGER_Z==twv::vita::ScriptTriggerZ &&
              PAD_TRIGGER_L==twv::vita::ScriptTriggerL && PAD_TRIGGER_R==twv::vita::ScriptTriggerR &&
              PAD_BUTTON_UP==twv::vita::ScriptButtonUp && PAD_BUTTON_DOWN==twv::vita::ScriptButtonDown &&
              PAD_BUTTON_LEFT==twv::vita::ScriptButtonLeft && PAD_BUTTON_RIGHT==twv::vita::ScriptButtonRight);
twv::vita::InputScript script;
bool active=false;
unsigned reads=0,lastStep=~0u,lastLoops=0;
}

extern "C" int twv_vita_input_script_load(const char* path) {
    static char text[65536];
    FILE* file=std::fopen(path,"rb");
    if(!file)return 0;
    const auto n=std::fread(text,1,sizeof text-1,file);
    const bool tooLong=n==sizeof text-1 && std::fgetc(file)!=EOF;
    std::fclose(file);
    if(tooLong)return 0;
    text[n]=0;
    if(!script.parse(text))return 0;
    active=true;
    twv::vita::telemetryEvent("input_script_steps",script.count);
    return 1;
}

extern "C" void twv_vita_benchmark_pad(PADStatus* status) {
    if(!active)return;
    // Page keys wait for the settings page (it opens at the next pass): a read before then (a second
    // tick in the same pass) has no input. After 300 such reads (the page never opened) one is
    // dropped, so the script cannot stall.
    static unsigned waitingForPage=0;
    if(script.page_pending() && waitingForPage<300) {
        ++waitingForPage;
        auto& p=status[0];p={};p.err=PAD_ERR_NONE;
        return;
    }
    waitingForPage=0;
    const auto* step=script.next();
    if(!step) {
        active=false;
        twv::vita::telemetryEvent("input_script_done",reads);
        return;
    }
    if(script.loops!=lastLoops){lastLoops=script.loops;twv::vita::telemetryEvent("input_script_loops",script.loops);}
    if(script.index!=lastStep){lastStep=script.index;twv::vita::telemetryEvent("input_script_step",script.index);}
    ++reads;
    if(step->kind==twv::vita::StepWarp) {
        // The request a loading zone makes; the play scene takes it on its next execute.
        char text[48];
        std::snprintf(text,sizeof text,"%s,%d,%d,%d",step->stage,step->room,step->point,step->layer);
        twv::vita::telemetryText("input_script_warp",text);
        dComIfGp_setNextStage(step->stage,s16(step->point),s8(step->room),s8(step->layer));
    } else if(step->kind==twv::vita::StepDarkClear && script.used==1) {
        twv::vita::telemetryEvent("input_script_darkclear",step->layer);
        dComIfGs_onDarkClearLV(step->layer);
    }
    auto& p=status[0];p={};p.err=PAD_ERR_NONE;
    if(step->page) {
        // "page open": this read has no input and the settings page opens at the next pass. A page key
        // reaching a pad read means the page was not open to read it: logged, no input.
        if(step->page==twv::vita::ScriptPageOpen && twv_vita_settings_request_open) {
            twv_vita_settings_request_open();
            twv::vita::telemetryEvent("input_script_page_open",reads);
        } else if(step->page==twv::vita::ScriptPageGesture && twv_vita_settings_inject_rear_hold) {
            twv_vita_settings_inject_rear_hold();
            twv::vita::telemetryEvent("input_script_page_gesture",reads);
        } else {
            twv::vita::telemetryEvent("input_script_page_key_unread",step->page);
        }
        return;
    }
    p.button=static_cast<u16>(twv::vita::resolve_vita_buttons(step->buttons,twv_vita_confirm_circle()!=0));
    p.stickX=static_cast<s8>(step->x);p.stickY=static_cast<s8>(step->y);
    p.triggerLeft=(step->buttons&twv::vita::ScriptTriggerL)?255:0;
    p.triggerRight=(step->buttons&twv::vita::ScriptTriggerR)?255:0;
}

// The open settings page (ui/settings_screen.cpp) reads its keys here, one per call.
extern "C" unsigned twv_vita_input_script_page_key(void) {
    if(!active)return 0;
    const uint32_t key=script.page_key();
    if(key) {
        if(script.index!=lastStep){lastStep=script.index;twv::vita::telemetryEvent("input_script_step",script.index);}
        twv::vita::telemetryEvent("input_script_page_key",key);
    }
    return key;
}
