// SDK domain: the settings page on the Vita (ui/settings_screen.hpp). The CPU draws the page into
// its own 960x544 buffer and sceDisplay shows it, as the start-up screens do (no GXM scene, no
// shader); the game is paused around it by aurora/aurora_vita.cpp.
#include "ui/settings_screen.hpp"

#include "gfx_vgl/frame_capture.hpp"
#include "runtime/player_settings.hpp"
#include "runtime/telemetry.hpp"
#include "runtime/visual_skips.hpp"
#include "ui/settings_draw.hpp"
#include "ui/settings_gesture.hpp"
#include "ui/settings_page.hpp"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdio>
#include <cstring>

#if __has_include("twv_build_version.h")
#include "twv_build_version.h"
#endif
#ifndef TWV_BUILD_VERSION
#define TWV_BUILD_VERSION "unknown build"
#endif

// The game side (dusk/settings_bridge.cpp) is always linked: strong references (settings_screen.hpp)
// pull it in. Weak: pieces some builds leave out; the entries they serve are then not shown.
extern "C" int twv_gxm_fps_overlay(int) __attribute__((weak));
extern "C" void twv_vita_scene_snapshot(const char*) __attribute__((weak));
extern "C" unsigned twv_vita_input_script_page_key(void) __attribute__((weak));
// runtime/player_settings.cpp (the X/O swap): 1 when circle confirms.
extern "C" int twv_vita_confirm_circle(void) __attribute__((weak));

namespace twv::vita {
namespace {
namespace st = twv::ui::settings;

constexpr uint32_t ScreenBytes = st::Width * st::Height * 4;
constexpr uint32_t GameButtons = 0xFFFFu; // every Vita button the game or the page reads
constexpr int TapSlop = 24;               // pixels a tap may slide

struct Screen {
    // Configuration (main_vita.cpp).
    VitaSettings inForce, stored;
    char disc[48] = "Disc unknown";
    char discId[8] = "";
    char build[96] = "";
    char hint[96] = "";
    bool configured = false;
    // Gesture and requests.
    ui::OpenGestureRecogniser gesture;
    bool requested = false;
    // The open page.
    bool open = false, closing = false, dirty = false;
    st::Page page;
    st::KeyRepeat repeat;
    SceUID block = -1;
    uint32_t* pixels = nullptr;
    SceDisplayFrameBuf saved{}; // the game's frame on the display when the page opened
    bool haveSaved = false;
    st::Backdrop backdrop;
    st::PageText text;
    uint64_t openedAt = 0;
    unsigned frames = 0, scriptWait = 0, shots = 0, changes = 0;
    bool gameChanged = false, portChanged = false, restartFailed = false;
    // Front taps while open: fingers that landed after the page opened.
    struct Tap { unsigned long long id; float x0, y0, x, y; bool live; } taps[4]{};
    int pendingTapX = -1, pendingTapY = -1;
} s;

const char* gesture_hint(OpenGesture g) {
    switch (g) {
    case OpenGesture::Front: return "Settings: touch the screen with three fingers";
    case OpenGesture::Both: return "Settings: two fingers held on the rear pad, or three on the screen";
    default: return "Settings: hold two fingers on the rear touch pad for 1 second";
    }
}

void apply_gesture(OpenGesture g) {
    s.gesture.rear = g != OpenGesture::Front;
    s.gesture.front = g != OpenGesture::Rear;
    std::snprintf(s.hint, sizeof s.hint, "%s", gesture_hint(g));
}

bool confirm_on_circle() { return twv_vita_confirm_circle && twv_vita_confirm_circle(); }

void* alloc_buffer() {
    const struct {SceKernelMemBlockType type; uint32_t align;} kinds[] = {
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256u * 1024u},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1024u * 1024u}};
    for (const auto& kind : kinds) {
        const uint32_t size = (ScreenBytes + kind.align - 1) & ~(kind.align - 1);
        const SceUID block = sceKernelAllocMemBlock("twv_settings_page", kind.type, size, nullptr);
        if (block < 0) {
            telemetryEvent("settings_page_alloc_result", block);
            continue;
        }
        void* base = nullptr;
        if (sceKernelGetMemBlockBase(block, &base) >= 0 && base) {
            s.block = block;
            return base;
        }
        sceKernelFreeMemBlock(block);
    }
    return nullptr;
}

int show(void* base) {
    SceDisplayFrameBuf fb{};
    fb.size = sizeof fb;
    fb.base = base;
    fb.pitch = st::Width;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = st::Width;
    fb.height = st::Height;
    return sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

ui::Canvas canvas() { return {s.pixels, st::Width, st::Height, st::Width}; }

void draw() {
    s.text.releaseWait = s.closing;
    st::draw_page(canvas(), s.page, s.backdrop, s.text);
    s.dirty = false;
}

// --- values: page choice index <-> Dusklight setting or port setting ---
int index_of(const int* values, int count, int value) {
    int best = 0;
    for (int i = 0; i < count; ++i) {
        const int d = values[i] - value, b = values[best] - value;
        if ((d < 0 ? -d : d) < (b < 0 ? -b : b)) best = i;
    }
    return best;
}
int game_key(st::Id id) {
    switch (id) {
    case st::Id::ButtonPrompts: return TwvGameButtonPrompts;
    case st::Id::HudButtons: return TwvGameHudButtons;
    case st::Id::ConfirmButton: return TwvGameConfirm;
    case st::Id::Language: return TwvGameLanguage;
    case st::Id::GyroAim: return TwvGameGyroAim;
    case st::Id::GyroSensitivity: return TwvGameGyroSensitivity;
    case st::Id::GyroInvertYaw: return TwvGameGyroInvertYaw;
    case st::Id::GyroInvertPitch: return TwvGameGyroInvertPitch;
    case st::Id::FreeCamera: return TwvGameFreeCamera;
    case st::Id::CameraInvertX: return TwvGameCameraInvertX;
    case st::Id::CameraInvertY: return TwvGameCameraInvertY;
    case st::Id::GyroRollgoal: return TwvGameGyroRollgoal;
    case st::Id::AimInvertX: return TwvGameAimInvertX;
    case st::Id::AimInvertY: return TwvGameAimInvertY;
    case st::Id::AutoSave: return TwvGameAutoSave;
    case st::Id::InstantText: return TwvGameInstantText;
    case st::Id::HoldToMash: return TwvGameHoldToMash;
    case st::Id::QuickTransform: return TwvGameQuickTransform;
    case st::Id::FastClimbing: return TwvGameFastClimbing;
    case st::Id::FastTears: return TwvGameFastTears;
    case st::Id::FastTransitions: return TwvGameFastTransitions;
    case st::Id::InstantSaves: return TwvGameInstantSaves;
    case st::Id::RupeeMessages: return TwvGameRupeeMessages;
    case st::Id::RupeesAlwaysFit: return TwvGameRupeesAlwaysFit;
    case st::Id::BiggerWallets: return TwvGameBiggerWallets;
    case st::Id::AimingReticle: return TwvGameAimingReticle;
    case st::Id::LowHealthBeep: return TwvGameLowHealthBeep;
    case st::Id::HideHud: return TwvGameHideHud;
    case st::Id::HudSize: return TwvGameHudSize;
    case st::Id::DamageTaken: return TwvGameDamageTaken;
    case st::Id::HeartDrops: return TwvGameHeartDrops;
    case st::Id::FastIronBoots: return TwvGameFastIronBoots;
    case st::Id::InfiniteHearts: return TwvGameInfiniteHearts;
    case st::Id::InfiniteArrows: return TwvGameInfiniteArrows;
    case st::Id::InfiniteBombs: return TwvGameInfiniteBombs;
    case st::Id::InfiniteRupees: return TwvGameInfiniteRupees;
    case st::Id::InfiniteOil: return TwvGameInfiniteOil;
    case st::Id::InfiniteAir: return TwvGameInfiniteAir;
    default: return -1;
    }
}
// Game value -> choice index.
int game_index(st::Id id, int value) {
    if (id == st::Id::Language) return index_of(st::LanguageValues, 6, value);
    if (id == st::Id::GyroSensitivity) return index_of(st::GyroSensitivityHundredths, 9, value);
    if (id == st::Id::HudSize) return index_of(st::HudSizeHundredths, 4, value);
    if (id == st::Id::DamageTaken) return index_of(st::DamageTakenValues, 3, value);
    return value;
}
int game_value(st::Id id, int index) {
    if (id == st::Id::Language) return st::LanguageValues[index];
    if (id == st::Id::GyroSensitivity) return st::GyroSensitivityHundredths[index];
    if (id == st::Id::HudSize) return st::HudSizeHundredths[index];
    if (id == st::Id::DamageTaken) return st::DamageTakenValues[index];
    return index;
}

// Languages on the disc (region letter of the game ID, runtime/player_settings.hpp): bit = choice
// index of text::Language.
void language_choices(uint32_t& mask, int& first) {
    const char region = s.discId[3]; // GZ2P01: P Europe, E USA, J Japan
    mask = 0;
    for (int i = 0; i < 6; ++i)
        if (disc_has_language(region, st::LanguageValues[i])) mask |= 1u << i;
    first = index_of(st::LanguageValues, 6, disc_first_language(region));
}

void load_values() {
    st::Page& p = s.page;
    p = st::Page{};
    p.index[int(st::Id::RenderScale)] = index_of(st::RenderScaleValues, 4, s.stored.renderScaleSet ? s.stored.renderScale : s.inForce.renderScale);
    p.index[int(st::Id::Performance)] = (s.stored.performanceSet ? s.stored.batterySaver : s.inForce.batterySaver) ? 1 : 0;
    p.index[int(st::Id::ShowFps)] = s.inForce.showFps ? 1 : 0;
    p.index[int(st::Id::OpenGesture)] = int(s.inForce.gesture);
    p.index[int(st::Id::SkipUnseen)] = s.inForce.skipUnseen ? 1 : 0;
    p.index[int(st::Id::StillAnimals)] = s.inForce.stillAnimals ? 1 : 0;
    p.index[int(st::Id::RestingAnimals)] = s.inForce.restingAnimals ? 1 : 0;
    p.index[int(st::Id::Crowd)] = s.inForce.crowd;
    p.available[int(st::Id::ShowFps)] = twv_gxm_fps_overlay && twv_gxm_fps_overlay(s.inForce.showFps);
    uint32_t mask = 1;
    int first = 0;
    language_choices(mask, first);
    p.set_allowed(st::Id::Language, mask, first);
    for (int i = 0; i < st::EntryCount; ++i) {
        const st::Id id = st::Entries[i].id;
        const int key = game_key(id);
        if (key < 0) continue;
        int value = 0, def = 0;
        const bool have = twv_dusk_setting_get(key, &value);
        p.available[i] = have;
        if (!have) continue;
        if (twv_dusk_setting_default(key, &def)) p.defaults[i] = game_index(id, def);
        p.index[i] = game_index(id, value);
        if (id == st::Id::Language && !p.allowed(id, p.index[i])) p.index[i] = first;
    }
    // Dusklight's default language (English) is not on a Japanese disc: the disc's own is the default.
    if (!p.allowed(st::Id::Language, p.defaults[int(st::Id::Language)])) p.defaults[int(st::Id::Language)] = first;
    // The recentre goes with the gyro entries; the preset with the two entries it sets.
    p.available[int(st::Id::GyroRecentre)] = p.available[int(st::Id::GyroAim)];
    p.available[int(st::Id::RestartGame)] = !s.restartFailed;
    p.available[int(st::Id::Preset)] = p.available[int(st::Id::RenderScale)] && p.available[int(st::Id::Performance)];
    p.sync_preset();
    p.sync_speed();
}

void restart_game();

void apply(const st::Effect& e) {
    switch (e.type) {
    case st::Effect::Changed: {
        ++s.changes;
        char text[96];
        std::snprintf(text, sizeof text, "%s=%s", st::entry(e.id).label, st::entry(e.id).choices[e.index]);
        telemetryText("settings_change", text);
        const int key = game_key(e.id);
        if (key >= 0) {
            const int ok = twv_dusk_setting_set(key, game_value(e.id, e.index));
            if (!ok) telemetryText("settings_change_failed", st::entry(e.id).label);
            s.gameChanged = true;
            return;
        }
        s.portChanged = true;
        // Back at the default: not written, so the built-in default keeps applying.
        const bool chosen = e.index != s.page.defaults[int(e.id)];
        switch (e.id) {
        case st::Id::RenderScale:
            s.stored.renderScale = st::RenderScaleValues[e.index];
            s.stored.renderScaleSet = chosen;
            break;
        case st::Id::Performance:
            s.stored.batterySaver = e.index == 1;
            s.stored.performanceSet = chosen;
            break;
        case st::Id::ShowFps:
            s.stored.showFps = s.inForce.showFps = e.index == 1;
            s.stored.showFpsSet = chosen;
            if (twv_gxm_fps_overlay) twv_gxm_fps_overlay(s.inForce.showFps);
            break;
        case st::Id::OpenGesture:
            s.stored.gesture = s.inForce.gesture = OpenGesture(e.index);
            s.stored.gestureSet = chosen;
            apply_gesture(s.inForce.gesture);
            break;
        // Speed-ups (runtime/visual_skips.hpp): in force at once.
        case st::Id::SkipUnseen:
            s.stored.skipUnseen = s.inForce.skipUnseen = e.index == 1;
            s.stored.skipUnseenSet = chosen;
            twv_vita_visual_skip_select(SkipHiddenDraw, e.index);
            break;
        case st::Id::StillAnimals:
            s.stored.stillAnimals = s.inForce.stillAnimals = e.index == 1;
            s.stored.stillAnimalsSet = chosen;
            twv_vita_visual_skip_select(SkipOffscreenAnim, e.index);
            break;
        case st::Id::RestingAnimals:
            s.stored.restingAnimals = s.inForce.restingAnimals = e.index == 1;
            s.stored.restingAnimalsSet = chosen;
            twv_vita_visual_skip_select(RestCreatures, e.index);
            break;
        case st::Id::Crowd:
            s.stored.crowd = s.inForce.crowd = e.index;
            s.stored.crowdSet = chosen;
            twv_vita_crowd_select(e.index);
            break;
        default: break;
        }
        return;
    }
    case st::Effect::Recentre:
        twv_dusk_gyro_recentre();
        telemetryEvent("settings_gyro_recentre", 1);
        return;
    case st::Effect::Close:
        s.closing = true;
        return;
    case st::Effect::Restart:
        restart_game();
        return;
    default: return;
    }
}

uint32_t held_keys(uint32_t buttons) {
    const bool circle = confirm_on_circle();
    uint32_t k = 0;
    if (buttons & SCE_CTRL_UP) k |= st::KeyUp;
    if (buttons & SCE_CTRL_DOWN) k |= st::KeyDown;
    if (buttons & SCE_CTRL_LEFT) k |= st::KeyLeft;
    if (buttons & SCE_CTRL_RIGHT) k |= st::KeyRight;
    if (buttons & (circle ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS)) k |= st::KeyConfirm;
    if (buttons & (circle ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE)) k |= st::KeyBack;
    if (buttons & SCE_CTRL_TRIANGLE) k |= st::KeyDefault;
    if (buttons & SCE_CTRL_START) k |= st::KeyStart;
    if (buttons & SCE_CTRL_LTRIGGER) k |= st::KeySectionPrev;
    if (buttons & SCE_CTRL_RTRIGGER) k |= st::KeySectionNext;
    return k;
}

uint32_t buttons_now() {
    SceCtrlData pad{};
    return sceCtrlPeekBufferPositive(0, &pad, 1) > 0 ? pad.buttons & GameButtons : 0;
}

void shot() {
    char path[96];
    std::snprintf(path, sizeof path, "%s/settings_page_%u.png", DataRoot, ++s.shots);
    twv::gpu::write_png_rgba(path, reinterpret_cast<const unsigned char*>(s.pixels), st::Width, st::Height,
                             st::Width * 4, false);
    telemetryText("settings_page_shot", path);
}

void save_changes() {
    if (s.gameChanged) {
        const int ok = twv_dusk_settings_save();
        telemetryEvent("settings_config_saved", ok);
    }
    if (s.portChanged) {
        const bool ok = write_vita_settings(s.stored, VitaSettingsFile, VitaSettingsTemp);
        telemetryEvent("settings_vita_settings_saved", ok);
    }
}

// "Restart the game": what changed is saved, then the application starts again from its own
// executable. If the system refuses, the entry goes away and the page says what to do instead.
void restart_game() {
    save_changes();
    s.gameChanged = s.portChanged = false;
    telemetryEvent("settings_restart_requested", 1);
    telemetryFlush();
    telemetrySync();
    const int result = sceAppMgrLoadExec("app0:eboot.bin", nullptr, nullptr);
    // Accepted: the system ends this process shortly. Still here after five seconds: it did not.
    for (int pass = 0; result >= 0 && pass < 300; ++pass) sceKernelDelayThread(16000);
    telemetryEvent("settings_restart_failed", result);
    telemetryFlush();
    s.restartFailed = true;
    s.page.available[int(st::Id::RestartGame)] = false;
    s.page.handle(st::KeyDown);
    s.page.status = "The system refused the restart: close the game from the home screen and start it again.";
}

void finish_close() {
    // The game's last frame back on the display, then our buffer can go once it is no longer shown.
    if (s.haveSaved) telemetryEvent("settings_page_restore_display", sceDisplaySetFrameBuf(&s.saved, SCE_DISPLAY_SETBUF_NEXTFRAME));
    sceDisplayWaitVblankStart();
    if (s.block >= 0) sceKernelFreeMemBlock(s.block);
    s.block = -1;
    s.pixels = nullptr;
    save_changes();
    if (twv_vita_scene_snapshot) twv_vita_scene_snapshot("settings_close");
    telemetryEvent("settings_page_changes", s.changes);
    telemetryEvent("settings_page_frames", s.frames);
    telemetryEvent("settings_page_open_ms", static_cast<long long>((processTimeUs() - s.openedAt) / 1000));
    telemetryEvent("settings_page_closed", 1);
    telemetryFlush();
    s.open = s.closing = false;
    s.gesture.reset();
}

} // namespace

void settingsConfigure(const VitaSettings& inForce, const VitaSettings& stored, const char* discId) {
    s.inForce = inForce;
    s.stored = stored;
    if (discId && std::strlen(discId) >= 4) {
        std::snprintf(s.discId, sizeof s.discId, "%s", discId);
        const char region = discId[3];
        std::snprintf(s.disc, sizeof s.disc, "Disc %s (%s)", discId,
                      region == 'P' ? "Europe" : region == 'J' ? "Japan" : region == 'E' ? "USA" : "?");
    }
    std::snprintf(s.build, sizeof s.build, "%s", TWV_BUILD_VERSION);
    apply_gesture(inForce.gesture);
    if (twv_gxm_fps_overlay) twv_gxm_fps_overlay(inForce.showFps);
    s.configured = true;
    telemetryText("build_version", s.build);
    telemetryText("settings_open_gesture", OpenGestureNames[int(inForce.gesture)]);
}

// Rear pad receipts: the first finger seen (rear touches reach the page at all), then one line per
// attempt (every finger lifted) with the recogniser's counters, at most 40 per run.
static void rear_receipts(int phase, float x, float y) {
    static bool seen = false;
    static unsigned attempts = 0;
    if (phase == 0 && !seen) {
        seen = true;
        char text[48];
        std::snprintf(text, sizeof text, "x=%.2f y=%.2f", double(x), double(y));
        telemetryText("settings_gesture_rear_seen", text);
    }
    if (!s.gesture.take_rear_attempt_end() || attempts >= 40) return;
    ++attempts;
    const auto& r = s.gesture.rearStats;
    char text[192];
    std::snprintf(text, sizeof text,
                  "downs=%u max_fingers=%u pairs=%u opened=%u apart=%u slid=%u short=%u last_apart=%.2f,%.2f last_slide=%.2f",
                  r.downs, r.maxFingers, r.pairs, r.opened, r.apart, r.slid, r.shortHold, double(r.lastApartX),
                  double(r.lastApartY), double(r.lastSlide));
    telemetryText("settings_gesture_rear", text);
}

const char* settingsOpenHint() { return s.configured ? s.hint : gesture_hint(OpenGesture::Both); }

} // namespace twv::vita

using twv::vita::s;
namespace st = twv::ui::settings;

extern "C" void twv_settings_touch(int panel, int phase, unsigned long long id, float x, float y) {
    if (panel != 1 && panel != 2) return;
    if (!s.open) {
        s.gesture.touch(twv::ui::TouchPanel(panel), twv::ui::TouchPhase(phase), id, x, y, twv::vita::processTimeUs());
        if (panel == 2) twv::vita::rear_receipts(phase, x, y);
        return;
    }
    if (panel != 1 || s.closing) return;
    // Taps: a front finger that lands on the open page and lifts close to where it landed.
    auto* taps = s.taps;
    if (phase == 0) {
        for (auto& t : s.taps)
            if (!t.live) {
                t = {id, x, y, x, y, true};
                return;
            }
        return;
    }
    for (int i = 0; i < 4; ++i) {
        auto& t = taps[i];
        if (!t.live || t.id != id) continue;
        t.x = x;
        t.y = y;
        if (phase == 2 || phase == 3) {
            t.live = false;
            const float dx = (t.x - t.x0) * st::Width, dy = (t.y - t.y0) * st::Height;
            if (phase == 2 && dx < twv::vita::TapSlop && dx > -twv::vita::TapSlop && dy < twv::vita::TapSlop &&
                dy > -twv::vita::TapSlop) {
                s.pendingTapX = int(t.x0 * st::Width);
                s.pendingTapY = int(t.y0 * st::Height);
            }
        }
        return;
    }
}

extern "C" void twv_vita_settings_request_open(void) { s.requested = true; }

// Test hook (the input script's "page gesture"): two fingers land together on the rear pad and stay
// there; the recogniser opens the page after its hold time, as for real fingers. They are forgotten
// when the page closes (the recogniser is reset).
extern "C" void twv_vita_settings_inject_rear_hold(void) {
    if (s.open) return;
    const uint64_t now = twv::vita::processTimeUs();
    s.gesture.touch(twv::ui::TouchPanel::Rear, twv::ui::TouchPhase::Down, 0x7E570001ull, 0.45f, 0.55f, now);
    s.gesture.touch(twv::ui::TouchPanel::Rear, twv::ui::TouchPhase::Down, 0x7E570002ull, 0.56f, 0.53f, now);
    twv::vita::telemetryEvent("settings_gesture_injected", s.gesture.rear);
}

extern "C" int twv_settings_poll_open(void) {
    if (s.open) return 0;
    const bool gesture = s.gesture.poll(twv::vita::processTimeUs());
    const bool requested = s.requested;
    s.requested = false;
    if (gesture || requested) twv::vita::telemetryText("settings_page_request", gesture ? "gesture" : "script");
    return gesture || requested;
}

extern "C" int twv_settings_is_open(void) { return s.open; }

extern "C" int twv_settings_open(void) {
    using namespace twv::vita;
    if (s.open) return 1;
    const uint64_t start = processTimeUs();
    // Every queued flip shown: nothing the GPU or the display queue still owns can replace our buffer.
    telemetryEvent("settings_page_display_queue_finish", sceGxmDisplayQueueFinish());
    s.saved = {};
    s.saved.size = sizeof s.saved;
    s.haveSaved = sceDisplayGetFrameBuf(&s.saved, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0 && s.saved.base;
    s.backdrop = {};
    if (s.haveSaved && s.saved.pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 && s.saved.width <= st::Width &&
        s.saved.height <= st::Height && s.saved.pitch >= s.saved.width)
        s.backdrop = {static_cast<const uint32_t*>(s.saved.base), int(s.saved.width), int(s.saved.height), int(s.saved.pitch)};
    void* base = alloc_buffer();
    if (!base) {
        telemetryText("settings_page_unavailable", "no memory for the page's display buffer");
        return 0;
    }
    s.pixels = static_cast<uint32_t*>(base);
    if (twv_vita_scene_snapshot) twv_vita_scene_snapshot("settings_open");
    load_values();
    s.text.build = s.build;
    s.text.disc = s.disc;
    s.text.opens = s.hint;
    s.text.confirm = confirm_on_circle() ? "O" : "X";
    s.text.back = confirm_on_circle() ? "X" : "O";
    s.open = true;
    s.closing = false;
    s.frames = s.changes = 0;
    s.gameChanged = s.portChanged = false;
    s.scriptWait = 2;
    for (auto& t : s.taps) t.live = false;
    s.pendingTapX = s.pendingTapY = -1;
    // Buttons already held do not count as presses.
    s.repeat = {};
    s.repeat.last = held_keys(buttons_now());
    draw();
    const int shown = show(base);
    s.openedAt = processTimeUs();
    telemetryEvent("settings_page_display", shown);
    telemetryEvent("settings_page_backdrop", s.backdrop.pixels != nullptr);
    if (s.haveSaved) telemetryEvent("settings_page_game_frame_width", s.saved.width);
    telemetryEvent("settings_page_open_us", static_cast<long long>(s.openedAt - start));
    telemetryFlush();
    return 1;
}

extern "C" int twv_settings_frame(void) {
    using namespace twv::vita;
    if (!s.open) return 0;
    ++s.frames;
    const uint32_t buttons = buttons_now();
    if (s.closing) {
        // Resume only once every button is up: nothing pressed on the page reaches the game.
        if (!buttons) {
            finish_close();
            return 0;
        }
        if (!s.text.releaseWait) draw();
        sceDisplayWaitVblankStart();
        return 1;
    }
    uint32_t pressed = s.repeat.update(held_keys(buttons), processTimeUs());
    // Test script keys: one every third pass (pressed, then two passes without); "page wait" steps
    // one per pass.
    if (twv_vita_input_script_page_key && s.scriptWait-- == 0) {
        const unsigned key = twv_vita_input_script_page_key();
        s.scriptWait = key && key != TwvPageWait ? 2 : 0;
        if (key == TwvPageShot) shot();
        else if (key != TwvPageWait) pressed |= key;
    }
    st::Effects effects = s.page.handle(pressed);
    if (s.pendingTapX >= 0) {
        const st::Effects tapped = s.page.tap(s.pendingTapX, s.pendingTapY);
        for (int i = 0; i < tapped.count; ++i) effects.add(tapped.list[i]);
        s.pendingTapX = s.pendingTapY = -1;
        s.dirty = true;
    }
    for (int i = 0; i < effects.count; ++i) apply(effects.list[i]);
    if (pressed || effects.count) s.dirty = true;
    if (s.dirty) draw();
    sceDisplayWaitVblankStart();
    return 1;
}
