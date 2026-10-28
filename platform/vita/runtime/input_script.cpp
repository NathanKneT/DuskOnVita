// Test-only scripted pad input (runtime/input_script.hpp), on patches/dusklight/0010's pad hook,
// which the opt-in benchmark fixture uses in benchmark builds (CMake keeps the two apart).
// Built with the game's int-enum ABI (dolphin PADStatus). Inactive unless main_vita.cpp loaded a
// script; once the script ends, the real pad is read untouched again.
#include "input_script.hpp"
#include "telemetry.hpp"
#include <dolphin/pad.h>
#include <cstdio>

namespace {
static_assert(PAD_BUTTON_A==twv::vita::ScriptButtonA && PAD_BUTTON_B==twv::vita::ScriptButtonB &&
              PAD_BUTTON_X==twv::vita::ScriptButtonX && PAD_BUTTON_Y==twv::vita::ScriptButtonY &&
              PAD_BUTTON_START==twv::vita::ScriptButtonStart && PAD_TRIGGER_Z==twv::vita::ScriptTriggerZ &&
              PAD_TRIGGER_L==twv::vita::ScriptTriggerL && PAD_TRIGGER_R==twv::vita::ScriptTriggerR &&
              PAD_BUTTON_UP==twv::vita::ScriptButtonUp && PAD_BUTTON_DOWN==twv::vita::ScriptButtonDown &&
              PAD_BUTTON_LEFT==twv::vita::ScriptButtonLeft && PAD_BUTTON_RIGHT==twv::vita::ScriptButtonRight);
twv::vita::InputScript script;
bool active=false;
unsigned reads=0,lastStep=~0u;
}

extern "C" int twv_vita_input_script_load(const char* path) {
    static char text[16384];
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
    const auto* step=script.next();
    if(!step) {
        active=false;
        twv::vita::telemetryEvent("input_script_done",reads);
        return;
    }
    if(script.index!=lastStep){lastStep=script.index;twv::vita::telemetryEvent("input_script_step",script.index);}
    ++reads;
    auto& p=status[0];p={};p.err=PAD_ERR_NONE;
    p.button=static_cast<u16>(step->buttons);
    p.stickX=static_cast<s8>(step->x);p.stickY=static_cast<s8>(step->y);
    p.triggerLeft=(step->buttons&twv::vita::ScriptTriggerL)?255:0;
    p.triggerRight=(step->buttons&twv::vita::ScriptTriggerR)?255:0;
}
