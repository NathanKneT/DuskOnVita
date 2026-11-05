// Game side of the settings page (ui/settings_screen.hpp): reads and writes Dusklight's own
// settings, so config.json stays Dusklight's file, written by Dusklight's save (temporary file, then
// rename; patches/dusklight/0051 for the Vita). Built with the game's int-enum ABI; plain C entry
// points only. Called on the main thread while the page is open (the game paused).
//
// A change goes to the Value layer (setValue), as a choice in Dusklight's own menu does: it replaces
// a launch override (--cvar) for this run and is saved; Dusklight writes only Value-layer settings.
// vita.confirmButton is not a Dusklight setting: the X/O swap (runtime/player_settings.hpp) reads
// it from config.json before the game starts. It is registered here as a string setting so the page
// can change it and Dusklight's save keeps writing it.
#include "ui/settings_screen.hpp"

#include "dusk/config.hpp"
#include "dusk/gyro.h"
#include "dusk/settings.h"

#include <cmath>
#include <string>

namespace {
using dusk::config::ConfigVar;

ConfigVar<std::string> g_confirmButton{"vita.confirmButton", std::string("system")};

// The language applies at the next start only: the game loads message archives by the current
// language as it goes (d_msg_object.cpp), so a change while it runs would mix two languages. The
// choice waits here and goes into config.json at each save; the running game keeps its language.
int g_pendingLanguage = -1;

void register_confirm() {
    if (dusk::config::GetConfigVar("vita.confirmButton") == nullptr) {
        dusk::config::Register(g_confirmButton); // takes a value config.json already held
    }
}

const char* const kConfirmNames[] = {"system", "cross", "circle"};

int confirm_index(const std::string& value) {
    for (int i = 0; i < 3; ++i) {
        if (value == kConfirmNames[i]) {
            return i;
        }
    }
    return 0;
}

bool valid_language(int v) {
    return (v >= 0 && v <= 4) || v == 6;
}

ConfigVar<bool>* flag(int key) {
    auto& g = dusk::getSettings().game;
    switch (key) {
    case TwvGameGyroAim: return &g.enableGyroAim;
    case TwvGameGyroInvertYaw: return &g.gyroInvertYaw;
    case TwvGameGyroInvertPitch: return &g.gyroInvertPitch;
    case TwvGameCameraInvertX: return &g.invertCameraXAxis;
    case TwvGameCameraInvertY: return &g.invertCameraYAxis;
    default: return nullptr;
    }
}

int hundredths(float v) {
    return static_cast<int>(std::lround(v * 100.0f));
}

// The value (current or default) of `key`; false for an unknown key.
bool read(int key, bool wantDefault, int& out) {
    auto& g = dusk::getSettings().game;
    if (ConfigVar<bool>* f = flag(key)) {
        out = (wantDefault ? f->getDefaultValue() : f->getValue()) ? 1 : 0;
        return true;
    }
    switch (key) {
    case TwvGameButtonPrompts: {
        const std::string& v = wantDefault ? g.buttonPrompts.getDefaultValue() : g.buttonPrompts.getValue();
        out = v == "gamecube" ? 1 : 0;
        return true;
    }
    case TwvGameConfirm:
        register_confirm();
        out = confirm_index(wantDefault ? g_confirmButton.getDefaultValue() : g_confirmButton.getValue());
        return true;
    case TwvGameLanguage:
        out = wantDefault ? static_cast<int>(g.language.getDefaultValue()) :
              g_pendingLanguage >= 0 ? g_pendingLanguage : static_cast<int>(g.language.getValue());
        return true;
    case TwvGameGyroSensitivity:
        // One entry for both axes: the sideways value stands for the pair.
        out = hundredths(wantDefault ? g.gyroSensitivityX.getDefaultValue() : g.gyroSensitivityX.getValue());
        return true;
    default: return false;
    }
}
}  // namespace

extern "C" int twv_dusk_setting_get(int key, int* value) {
    int v = 0;
    if (value == nullptr || !read(key, false, v)) {
        return 0;
    }
    *value = v;
    return 1;
}

extern "C" int twv_dusk_setting_default(int key, int* value) {
    int v = 0;
    if (value == nullptr || !read(key, true, v)) {
        return 0;
    }
    *value = v;
    return 1;
}

extern "C" int twv_dusk_setting_set(int key, int value) {
    auto& g = dusk::getSettings().game;
    if (ConfigVar<bool>* f = flag(key)) {
        f->setValue(value != 0);
        return 1;
    }
    switch (key) {
    case TwvGameButtonPrompts:
        g.buttonPrompts.setValue(std::string(value ? "gamecube" : "playstation"));
        return 1;
    case TwvGameConfirm:
        if (value < 0 || value > 2) {
            return 0;
        }
        register_confirm();
        g_confirmButton.setValue(std::string(kConfirmNames[value]));
        return 1;
    case TwvGameLanguage:
        if (!valid_language(value)) {
            return 0;
        }
        g_pendingLanguage = value == static_cast<int>(g.language.getValue()) ? -1 : value;
        return 1;
    case TwvGameGyroSensitivity:
        if (value < 5 || value > 1000) {
            return 0;
        }
        g.gyroSensitivityX.setValue(static_cast<float>(value) / 100.0f);
        g.gyroSensitivityY.setValue(static_cast<float>(value) / 100.0f);
        return 1;
    default: return 0;
    }
}

extern "C" int twv_dusk_settings_save(void) {
    auto& language = dusk::getSettings().game.language;
    if (g_pendingLanguage < 0) {
        dusk::config::save(); // logs its own failure (dusk::config); no result to return
        return 1;
    }
    // The chosen language is saved; the running game's language is put back as it was (a launch
    // override stays an override).
    const auto layer = language.getLayer();
    const dusk::GameLanguage running = language.getValue();
    language.setValue(static_cast<dusk::GameLanguage>(g_pendingLanguage));
    dusk::config::save();
    if (layer == dusk::config::ConfigVarLayer::Override) {
        language.setOverrideValue(running);
    } else {
        language.setValue(running); // the choice stays pending: every later save of the page writes it
    }
    return 1;
}

extern "C" void twv_dusk_gyro_recentre(void) {
    dusk::gyro::recentre(); // patches/dusklight/0050
}
