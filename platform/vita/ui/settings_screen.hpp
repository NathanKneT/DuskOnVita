#pragma once
// The settings page on the Vita (ui/settings_screen.cpp; model in ui/settings_page.hpp, drawing in
// ui/settings_draw.hpp). Plain C entry points: Aurora's glue (aurora/aurora_vita.cpp, int enums)
// opens it and runs it once per main-loop pass; the game side (dusk/settings_bridge.cpp) reads and
// writes Dusklight's settings for it.
//
// While the page is open the game is paused: aurora_vita.cpp finishes the frame in flight, the GXM
// display queue is emptied, the page shows its own 960x544 buffer (the game's last frame dimmed
// behind it), Aurora's game clock is held, the game loop gets AURORA_PAUSED (audio paused) and no
// frame begins, so no game tick, draw or pad read runs; SDL events are not passed to the game. On
// close the page waits until every button is released, puts the game's last frame back on the
// display, frees its buffer, saves what changed, and the loop gets AURORA_UNPAUSED (audio resumes,
// Dusklight's game clock restarts without catching up).
#include "runtime/vita_settings.hpp"

#include <cstdint>

namespace twv::vita {
// main_vita.cpp, before game_main: the port settings in force (args.txt, vita_settings.json,
// defaults), what vita_settings.json held (written back with the page's changes), and the disc.
void settingsConfigure(const VitaSettings& inForce, const VitaSettings& stored, const char* discId);
// The line the start-up "Loading" screen shows: how to open the page.
const char* settingsOpenHint();
} // namespace twv::vita

// Dusklight setting keys exchanged with the game side (plain ints across the enum domains).
enum : int {
    TwvGameButtonPrompts = 0, // 0 playstation, 1 gamecube           (game.buttonPrompts)
    TwvGameConfirm = 1,       // 0 system, 1 cross, 2 circle         (vita.confirmButton)
    TwvGameLanguage = 2,      // GameLanguage: 0 en 1 de 2 fr 3 es 4 it 6 ja (game.language)
    TwvGameGyroAim = 3,       // 0/1                                 (game.enableGyroAim)
    TwvGameGyroSensitivity = 4, // hundredths                        (game.gyroSensitivityX and Y)
    TwvGameGyroInvertYaw = 5, // 0/1                                 (game.gyroInvertYaw)
    TwvGameGyroInvertPitch = 6, // 0/1                               (game.gyroInvertPitch)
    TwvGameCameraInvertX = 7, // 0/1                                 (game.invertCameraXAxis)
    TwvGameCameraInvertY = 8, // 0/1                                 (game.invertCameraYAxis)
    TwvGameFreeCamera = 9,    // 0/1                                 (game.freeCamera)
    TwvGameHudButtons = 10,   // 0 vita, 1 original                  (game.hudButtonLayout)
    TwvGameKeyCount = 11
};

extern "C" {
// Aurora glue, main thread.
// A finger event of either panel (1 front, 2 rear; phase 0 down, 1 motion, 2 up, 3 cancelled;
// x, y in 0..1). Feeds the opening gesture, or the page's taps while it is open.
void twv_settings_touch(int panel, int phase, unsigned long long id, float x, float y);
// 1 when the gesture (or a test request) asks for the page; call once per pass while it is closed.
int twv_settings_poll_open(void);
// Opens the page; the caller has finished the frame in flight. 1 opened, 0 not (receipt says why).
int twv_settings_open(void);
// One pass of the open page: input, drawing, a vertical blank. 1 still open, 0 closed in this pass.
int twv_settings_frame(void);
int twv_settings_is_open(void);
// Test hooks (the input script's "page open"): open at the next pass, as the gesture would;
// ("page gesture"): two still fingers on the rear pad, for the gesture recogniser itself.
void twv_vita_settings_request_open(void);
void twv_vita_settings_inject_rear_hold(void);

// Game side (dusk/settings_bridge.cpp, twv_dusk). 1 when the key exists and the call worked.
int twv_dusk_setting_get(int key, int* value);
int twv_dusk_setting_default(int key, int* value);
int twv_dusk_setting_set(int key, int value);
int twv_dusk_settings_save(void); // config.json through Dusklight's own save (temporary file + rename)
void twv_dusk_gyro_recentre(void);
// Before the page opens: a front finger the game's menu pointer follows is cancelled (touch.cpp).
void twv_vita_front_touch_cancel(void);
// GXM backend (gfx_gxm/gxm_backend.cpp): the FPS counter on/off; returns 1 when GXM draws the game.
int twv_gxm_fps_overlay(int on);
// Scene probe (runtime/scene_probe.cpp, perf builds): game tick, counters and Link at this moment.
void twv_vita_scene_snapshot(const char* tag);
// Input script (runtime/input_script.cpp): the next "page KEY" step, consumed (0 when the next step
// is not a page key). Keys: settings_page.hpp Key bits, or TwvPageShot.
unsigned twv_vita_input_script_page_key(void);
}
constexpr unsigned TwvPageShot = 1u << 16; // input script "page SHOT": a PNG of the page
constexpr unsigned TwvPageWait = 1u << 17; // input script "page wait N": one pass without a key
