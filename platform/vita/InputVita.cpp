// Game/Aurora int-enum domain. SDL3's Vita driver remains the sole input source.
#include "InputVita.hpp"
#include "runtime/player_settings.hpp"
#include "runtime/telemetry.hpp"
#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>

static_assert(PAD_BUTTON_A == twv::vita::PadA && PAD_BUTTON_B == twv::vita::PadB, "GameCube A/B bits");

void twv::vita::configureInputDefaults() {
    // Vita has digital L/R but no physical L2/R2. Upstream Standard puts L/R
    // on the absent trigger axes and Z on R, which prevents normal targeting.
    // PAD's existing button path supplies full trigger pressure for L/R.
    // Cross (SOUTH) and circle (EAST) press A and B, or B and A when circle confirms
    // (runtime/player_settings.hpp: the console's Enter button or the player's setting).
    const bool circleConfirms = twv_vita_confirm_circle() != 0;
    const FaceButtons face = face_buttons(circleConfirms);
    const PADDefaultMapping mapping{
        {
            {SDL_GAMEPAD_BUTTON_SOUTH, static_cast<PADButton>(face.cross)},
            {SDL_GAMEPAD_BUTTON_EAST, static_cast<PADButton>(face.circle)},
            {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_X},
            {SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y},
            {SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START},
            {SDL_GAMEPAD_BUTTON_BACK, PAD_TRIGGER_Z},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_TRIGGER_L},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_R},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_BUTTON_UP},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_BUTTON_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_BUTTON_LEFT},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_BUTTON_RIGHT},
        },
        {
            {{SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_POS},
            {{SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_NEG},
            {{SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_POS},
            {{SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_NEG},
            {{SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_POS},
            {{SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_NEG},
            {{SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_POS},
            {{SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_NEG},
            {{SDL_GAMEPAD_AXIS_LEFT_TRIGGER, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_L},
            {{SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_R},
        }
    };
    PADSetDefaultMapping(&mapping, PAD_TYPE_STANDARD);
    twv::vita::telemetryText("input_defaults", circleConfirms
        ? "SDL Vita: dual sticks; circle=A cross=B square=X triangle=Y L=L R=R select=Z start=menu"
        : "SDL Vita: dual sticks; cross=A circle=B square=X triangle=Y L=L R=R select=Z start=menu");
}
