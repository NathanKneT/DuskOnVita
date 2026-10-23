// Game/Aurora int-enum domain. SDL3's Vita driver remains the sole input source.
#include "InputVita.hpp"
#include "runtime/telemetry.hpp"
#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>

void twv::vita::configureInputDefaults() {
    // Vita has digital L/R but no physical L2/R2. Upstream Standard puts L/R
    // on the absent trigger axes and Z on R, which prevents normal targeting.
    // PAD's existing button path supplies full trigger pressure for L/R.
    const PADDefaultMapping mapping{
        {
            {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A},
            {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_B},
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
    twv::vita::telemetryText("input_defaults", "SDL Vita: dual sticks; cross=A circle=B square=X triangle=Y L=L R=R select=Z start=menu");
}
