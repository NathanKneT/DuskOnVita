// The process-wide confirm choice (runtime/player_settings.hpp). Written once by main_vita.cpp
// before game_main; read afterwards by the pad mapping, the input script and the prompt glyphs.
#include "runtime/player_settings.hpp"

namespace {
int gConfirmCircle = 0;
}

extern "C" void twv_vita_set_confirm_circle(int circle) { gConfirmCircle = circle ? 1 : 0; }
extern "C" int twv_vita_confirm_circle(void) { return gConfirmCircle; }
