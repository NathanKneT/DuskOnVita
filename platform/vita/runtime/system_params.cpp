// The console's language and Enter button (Settings > System), read once before the game starts
// (main_vita.cpp; runtime/player_settings.hpp decides what they mean). SceAppUtil is started for the
// two reads and shut down again if this call started it, so SDL (which also uses it) finds it as
// it would without us.
#include <psp2/apputil.h>
#include <psp2/system_param.h>

#include <cstring>

extern "C" int twv_vita_system_params(int* language, int* enterButton, int* initResult) {
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    std::memset(&init, 0, sizeof init);
    std::memset(&boot, 0, sizeof boot);
    const int started = sceAppUtilInit(&init, &boot);
    int lang = -1, enter = -1;
    const int langResult = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    const int enterResult = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
    if (started >= 0) sceAppUtilShutdown();
    *language = langResult < 0 ? -1 : lang;
    *enterButton = enterResult < 0 ? -1 : enter;
    *initResult = started;
    return langResult >= 0 && enterResult >= 0;
}
