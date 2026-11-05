// Settings page model, drawing, gesture recogniser, vita_settings.json storage and the input
// script's page steps (ui/settings_page.hpp, ui/settings_draw.hpp, ui/settings_gesture.hpp,
// runtime/vita_settings.hpp, runtime/input_script.hpp). With an argument, also writes the drawn page
// as a binary PPM there (for a look at the layout without a Vita).
#include "runtime/input_script.hpp"
#include "runtime/vita_settings.hpp"
#include "ui/settings_draw.hpp"
#include "ui/settings_gesture.hpp"
#include "ui/settings_page.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace st = twv::ui::settings;
using twv::ui::OpenGestureRecogniser;
using twv::ui::TouchPanel;
using twv::ui::TouchPhase;

// The script's page keys are the page's keys (input_script.hpp cannot include the UI headers).
static_assert(twv::vita::ScriptPageUp == st::KeyUp && twv::vita::ScriptPageDown == st::KeyDown &&
              twv::vita::ScriptPageLeft == st::KeyLeft && twv::vita::ScriptPageRight == st::KeyRight &&
              twv::vita::ScriptPageConfirm == st::KeyConfirm && twv::vita::ScriptPageBack == st::KeyBack &&
              twv::vita::ScriptPageDefault == st::KeyDefault && twv::vita::ScriptPageStart == st::KeyStart);
static_assert(twv::vita::ScriptPageShot == (1u << 16) && twv::vita::ScriptPageWait == (1u << 17)); // TwvPageShot/Wait

namespace {
int idx(st::Id id) { return int(id); }

void entries() {
    std::set<std::string> labels;
    const int labelColumns = (st::ValueX - st::LabelX - 12) / (twv::ui::FontAdvance * st::RowScale);
    const int valueColumns = (st::ValueRight - st::ValueX - 2 * twv::ui::FontAdvance * st::RowScale - 12) /
                             (twv::ui::FontAdvance * st::RowScale);
    for (int i = 0; i < st::EntryCount; ++i) {
        const st::Entry& e = st::Entries[i];
        assert(int(e.id) == i);
        assert(labels.insert(e.label).second);
        assert(int(std::strlen(e.label)) <= labelColumns);
        if (e.kind == st::Kind::Choice) {
            assert(e.choices && e.count >= 2 && e.defaultIndex < e.count);
            for (int c = 0; c < e.count; ++c) assert(int(std::strlen(e.choices[c])) <= valueColumns);
        } else {
            assert(!e.choices && e.count == 0);
        }
        // Help with its "Applies at ..." prefix fits the help lines.
        const std::string help = std::string("Applies at the next start. ") + e.help;
        twv::ui::TextLine lines[4];
        bool cut = false;
        twv::ui::wrap_text(help.c_str(), std::size_t((st::ValueRight - st::LabelX) / (twv::ui::FontAdvance * st::SmallScale)),
                           lines, st::HelpLines, &cut);
        assert(!cut);
    }
    // The required entries, with their documented defaults.
    assert(st::entry(st::Id::RenderScale).count == 4 && st::RenderScaleValues[st::entry(st::Id::RenderScale).defaultIndex] == 75);
    assert(st::entry(st::Id::Performance).defaultIndex == 0); // Normal 444 MHz
    assert(st::entry(st::Id::ButtonPrompts).defaultIndex == 0); // PlayStation (the Vita's default)
    assert(st::entry(st::Id::GyroAim).defaultIndex == 1);       // on (patches/dusklight/0017)
    assert(st::GyroSensitivityHundredths[st::entry(st::Id::GyroSensitivity).defaultIndex] == 100);
    assert(st::entry(st::Id::RenderScale).applies == st::Applies::Restart);
    assert(st::entry(st::Id::Performance).applies == st::Applies::Restart);
    assert(st::entry(st::Id::Language).applies == st::Applies::Restart);
    assert(st::entry(st::Id::ConfirmButton).applies == st::Applies::Restart);
    assert(st::entry(st::Id::GyroInvertYaw).applies == st::Applies::Now);
    // Honest wording: 444 MHz is the standard maximum, not an overclock.
    assert(std::strstr(st::entry(st::Id::Performance).help, "no overclock"));
    std::puts("settings_page_test: entries, defaults, label/value/help sizes");
}

void navigation() {
    st::Page p;
    assert(p.selected == 0 && p.top == 0);
    // Up from the first entry wraps to the last; down wraps back.
    st::Effects e = p.handle(st::KeyUp);
    assert(p.selected == st::EntryCount - 1 && e.count == 0);
    assert(p.top + st::VisibleRows > p.selected && p.top <= p.selected);
    p.handle(st::KeyDown);
    assert(p.selected == 0 && p.top == 0);
    // Left/right wrap within the choices: render size 75 -> 100 -> 50, and back.
    p.handle(st::KeyDown);
    assert(p.selected == idx(st::Id::RenderScale));
    const int scale = idx(st::Id::RenderScale);
    e = p.handle(st::KeyRight);
    assert(e.count == 1 && e.list[0].type == st::Effect::Changed && e.list[0].id == st::Id::RenderScale &&
           e.list[0].index == 3);
    e = p.handle(st::KeyRight);
    assert(e.list[0].index == 0 && p.index[scale] == 0);
    e = p.handle(st::KeyLeft);
    assert(e.list[0].index == 3);
    // Confirm on a choice moves it on; triangle puts the default back.
    e = p.handle(st::KeyConfirm);
    assert(p.index[scale] == 0);
    e = p.handle(st::KeyDefault);
    assert(e.count == 1 && p.index[scale] == 2);
    e = p.handle(st::KeyDefault);
    assert(e.count == 0); // already the default: no change emitted
    // Back and START close.
    assert(p.handle(st::KeyBack).has(st::Effect::Close));
    assert(p.handle(st::KeyStart).has(st::Effect::Close));
    // Unavailable entries are skipped and not shown.
    st::Page q;
    q.available[idx(st::Id::Performance)] = false;
    q.selected = idx(st::Id::RenderScale);
    q.handle(st::KeyDown);
    assert(q.selected == idx(st::Id::ShowFps));
    int rows[st::VisibleRows];
    const int n = q.visible(rows);
    for (int r = 0; r < n; ++r) assert(rows[r] != idx(st::Id::Performance));
    // Scrolling: walking down keeps the selection drawn, and row_at finds it.
    st::Page s;
    for (int k = 0; k < st::EntryCount - 1; ++k) {
        s.handle(st::KeyDown);
        const int shown = s.visible(rows);
        int at = -1;
        for (int r = 0; r < shown; ++r)
            if (rows[r] == s.selected) at = r;
        assert(at >= 0);
        assert(s.row_at(st::ListTop + at * st::RowHeight + 5) == s.selected);
    }
    assert(s.more_above() && !s.more_below());
    s.handle(st::KeyDown); // wraps to the top
    assert(s.selected == 0 && s.top == 0 && !s.more_above() && s.more_below());
    std::puts("settings_page_test: navigation wraps, values wrap, default, close, hidden entries, scrolling");
}

void actions_and_language() {
    st::Page p;
    // Reset: the first confirm asks, the second resets, any other key in between disarms.
    p.selected = idx(st::Id::RenderScale);
    p.handle(st::KeyRight);               // render size changed
    p.selected = idx(st::Id::GyroAim);
    p.handle(st::KeyLeft);                // gyro aim off
    p.selected = idx(st::Id::ResetDefaults);
    st::Effects e = p.handle(st::KeyConfirm);
    assert(e.count == 0 && p.resetArmed && p.status);
    p.handle(st::KeyLeft);
    assert(!p.resetArmed);
    p.handle(st::KeyConfirm);
    e = p.handle(st::KeyConfirm);
    assert(!p.resetArmed && e.count == 2 && p.index[idx(st::Id::RenderScale)] == 2 && p.index[idx(st::Id::GyroAim)] == 1);
    assert(p.index[idx(st::Id::Preset)] == 1); // Balanced again
    // Recentre and Close are one press.
    p.selected = idx(st::Id::GyroRecentre);
    assert(p.handle(st::KeyConfirm).has(st::Effect::Recentre));
    p.selected = idx(st::Id::Close);
    assert(p.handle(st::KeyConfirm).has(st::Effect::Close));
    // Language: only the disc's languages; a PAL disc offers five, a USA disc one.
    st::Page pal;
    pal.set_allowed(st::Id::Language, 0x1F, 0);
    pal.selected = idx(st::Id::Language);
    pal.index[idx(st::Id::Language)] = 4; // Italian
    e = pal.handle(st::KeyRight);
    assert(e.list[0].index == 0); // wraps past Japanese (not on the disc)
    e = pal.handle(st::KeyLeft);
    assert(e.list[0].index == 4);
    st::Page usa;
    usa.index[idx(st::Id::Language)] = 3;
    usa.set_allowed(st::Id::Language, 0x1, 0);
    assert(usa.index[idx(st::Id::Language)] == 0 && usa.choice_count(st::Id::Language) == 1);
    usa.selected = idx(st::Id::Language);
    assert(usa.handle(st::KeyRight).count == 0);
    std::puts("settings_page_test: reset asks once, recentre, close, disc languages");
}

void taps() {
    st::Page p;
    // A tap on another entry's label chooses it without changing it.
    const int y2 = st::ListTop + 2 * st::RowHeight + 10;
    st::Effects e = p.tap(st::LabelX + 10, y2);
    assert(p.selected == 2 && e.count == 0);
    // On its value: the left half goes back, the right half forward.
    e = p.tap(st::ValueRight - 10, y2);
    assert(e.count == 1 && p.index[2] == 1);
    e = p.tap(st::ValueX + 5, y2);
    assert(e.count == 1 && p.index[2] == 0);
    // The footer's Close button, and taps outside everything.
    assert(p.tap(st::CloseButtonX + 5, st::CloseButtonY + 5).has(st::Effect::Close));
    assert(p.tap(5, 5).count == 0 && p.tap(st::LabelX, st::HelpY + 4).count == 0);
    // Reset by touch: two taps.
    p.top = st::EntryCount - st::VisibleRows;
    int rows[st::VisibleRows];
    const int n = p.visible(rows);
    int resetRow = -1;
    for (int r = 0; r < n; ++r)
        if (rows[r] == idx(st::Id::ResetDefaults)) resetRow = r;
    assert(resetRow >= 0);
    p.index[idx(st::Id::RenderScale)] = 0;
    const int yr = st::ListTop + resetRow * st::RowHeight + 10;
    assert(p.tap(st::LabelX, yr).count == 0 && p.resetArmed);
    assert(p.tap(st::LabelX, yr).count == 1 && p.index[idx(st::Id::RenderScale)] == 2);
    std::puts("settings_page_test: taps choose, change, close and reset");
}

// Presets set Render size and Performance together; any other pair shows Custom.
void presets() {
    const int preset = idx(st::Id::Preset), scale = idx(st::Id::RenderScale), perf = idx(st::Id::Performance);
    st::Page p;
    assert(p.index[preset] == 1 && p.defaults[preset] == 1); // Balanced: 75 %, 444 MHz
    p.selected = preset;
    st::Effects e = p.handle(st::KeyRight); // Battery
    assert(e.count == 3 && e.list[0].id == st::Id::Preset && e.list[0].index == 2 && e.list[1].id == st::Id::RenderScale &&
           e.list[1].index == 0 && e.list[2].id == st::Id::Performance && e.list[2].index == 1);
    e = p.handle(st::KeyRight); // past Custom (never chosen) to Quality
    assert(e.count == 3 && p.index[preset] == 0 && p.index[scale] == 3 && p.index[perf] == 0);
    // A size of its own: Custom, and the preset key steps on from there.
    p.selected = scale;
    e = p.handle(st::KeyLeft); // 75 %, 444 MHz: that is Balanced
    assert(e.count == 1 && p.index[preset] == 1);
    e = p.handle(st::KeyLeft); // 67 %
    assert(e.count == 1 && p.index[scale] == 1 && p.index[preset] == st::PresetCustom);
    p.selected = preset;
    e = p.handle(st::KeyRight);
    assert(p.index[preset] == 0 && p.index[scale] == 3 && p.index[perf] == 0);
    // Triangle: Balanced again (only the size moves back).
    e = p.handle(st::KeyDefault);
    assert(e.count == 2 && p.index[preset] == 1 && p.index[scale] == 2 && p.index[perf] == 0);
    // The battery saver on its own is Custom too; the shell derives the preset from stored values.
    p.selected = perf;
    p.handle(st::KeyRight);
    assert(p.index[preset] == st::PresetCustom);
    st::Page q;
    q.index[scale] = 0;
    q.index[perf] = 1;
    q.sync_preset();
    assert(q.index[preset] == 2);
    std::puts("settings_page_test: presets set size and clock, Custom when they match none");
}

void repeat() {
    st::KeyRepeat r;
    assert(r.update(st::KeyDown, 0) == st::KeyDown);
    assert(r.update(st::KeyDown, 100000) == 0);
    assert(r.update(st::KeyDown, 399999) == 0);
    assert(r.update(st::KeyDown, 400000) == st::KeyDown);
    assert(r.update(st::KeyDown, 450000) == 0);
    assert(r.update(st::KeyDown, 520000) == st::KeyDown);
    // Confirm never repeats; a held key that stays held at open is not a press.
    assert(r.update(st::KeyDown | st::KeyConfirm, 530000) == st::KeyConfirm);
    assert(r.update(st::KeyDown | st::KeyConfirm, 2000000) == st::KeyDown);
    st::KeyRepeat open;
    open.last = st::KeyBack;
    assert(open.update(st::KeyBack, 0) == 0 && open.update(0, 1) == 0 && open.update(st::KeyBack, 2) == st::KeyBack);
    std::puts("settings_page_test: D-pad repeat after 400 ms every 120 ms; held keys at open ignored");
}

constexpr uint64_t Ms = 1000;

void gesture() {
    OpenGestureRecogniser g;
    // Two fingers together on the rear pad, held 1 s: opens once.
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.40f, 0.50f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.55f, 0.52f, 200 * Ms);
    assert(!g.poll(900 * Ms) && !g.poll(1199 * Ms));
    assert(g.poll(1200 * Ms));
    assert(!g.poll(5000 * Ms)); // once, until every finger lifts
    g.touch(TouchPanel::Rear, TouchPhase::Up, 1, 0.40f, 0.50f, 5100 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Up, 2, 0.55f, 0.52f, 5100 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 3, 0.40f, 0.50f, 6000 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 4, 0.45f, 0.50f, 6100 * Ms);
    assert(g.poll(7100 * Ms));
    g.reset();
    // A finger of each hand (holding the Vita): far apart, never.
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.10f, 0.60f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.90f, 0.60f, 100 * Ms);
    assert(!g.poll(3000 * Ms));
    g.reset();
    // A finger resting there, then another one later: never.
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.40f, 0.50f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.45f, 0.50f, 600 * Ms);
    assert(!g.poll(3000 * Ms));
    g.reset();
    // Sliding spoils the pair.
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.40f, 0.50f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.45f, 0.50f, 50 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Motion, 2, 0.45f, 0.65f, 300 * Ms);
    assert(!g.poll(2000 * Ms));
    g.reset();
    // Other fingers on the pad do not get in the way (console b3: nothing opened while they did): a
    // finger resting there while holding the Vita, a third that comes and goes.
    g = OpenGestureRecogniser{}; // fresh counters
    g.touch(TouchPanel::Rear, TouchPhase::Down, 9, 0.92f, 0.85f, 0);          // resting since long before
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.40f, 0.50f, 10000 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.47f, 0.48f, 10200 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 3, 0.55f, 0.50f, 10230 * Ms); // a third, briefly
    g.touch(TouchPanel::Rear, TouchPhase::Up, 3, 0.55f, 0.50f, 10300 * Ms);
    assert(!g.poll(11100 * Ms) && g.poll(11200 * Ms) && !g.poll(12000 * Ms));
    assert(g.rearStats.opened == 1 && g.rearStats.maxFingers == 4 && g.down(TouchPanel::Rear) == 3);
    // Lifting one of the pair and landing another next to the one still held: a new pair.
    g.touch(TouchPanel::Rear, TouchPhase::Up, 2, 0.47f, 0.48f, 12100 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Up, 1, 0.40f, 0.50f, 12100 * Ms);
    assert(!g.take_rear_attempt_end()); // the resting finger is still down
    g.touch(TouchPanel::Rear, TouchPhase::Up, 9, 0.92f, 0.85f, 12200 * Ms);
    assert(g.take_rear_attempt_end() && !g.take_rear_attempt_end());
    g.reset();
    // The player's natural pair on the console (b3g receipts): about half the pad apart. Opens.
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.23f, 0.50f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.73f, 0.51f, 150 * Ms);
    assert(g.poll(1150 * Ms));
    g.reset();
    // Why pairs did not open the page: too far apart, slid, lifted early (the receipts' counters).
    {
        OpenGestureRecogniser c;
        c.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.10f, 0.60f, 0);
        c.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.90f, 0.60f, 100 * Ms);
        assert(!c.poll(2000 * Ms));
        c.touch(TouchPanel::Rear, TouchPhase::Up, 1, 0.10f, 0.60f, 2100 * Ms);
        c.touch(TouchPanel::Rear, TouchPhase::Up, 2, 0.90f, 0.60f, 2100 * Ms);
        c.touch(TouchPanel::Rear, TouchPhase::Down, 3, 0.40f, 0.50f, 3000 * Ms);
        c.touch(TouchPanel::Rear, TouchPhase::Down, 4, 0.45f, 0.50f, 3050 * Ms);
        c.touch(TouchPanel::Rear, TouchPhase::Motion, 4, 0.60f, 0.50f, 3300 * Ms);
        assert(!c.poll(5000 * Ms));
        c.touch(TouchPanel::Rear, TouchPhase::Up, 4, 0.60f, 0.50f, 5100 * Ms);
        c.touch(TouchPanel::Rear, TouchPhase::Up, 3, 0.40f, 0.50f, 5100 * Ms);
        c.touch(TouchPanel::Rear, TouchPhase::Down, 5, 0.40f, 0.50f, 6000 * Ms);
        c.touch(TouchPanel::Rear, TouchPhase::Down, 6, 0.45f, 0.50f, 6050 * Ms);
        assert(!c.poll(6500 * Ms));
        c.touch(TouchPanel::Rear, TouchPhase::Up, 6, 0.45f, 0.50f, 6600 * Ms);
        const auto& r = c.rearStats;
        assert(r.downs == 6 && r.pairs == 3 && r.apart == 1 && r.slid == 1 && r.shortHold == 1 && r.opened == 0);
        assert(r.lastApartX > 0.04f && r.lastApartX < 0.06f);
    }
    // A small wobble is fine.
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.40f, 0.50f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.45f, 0.50f, 50 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Motion, 1, 0.43f, 0.52f, 500 * Ms);
    assert(g.poll(1050 * Ms));
    g.reset();
    // Lifting before the second is up ends the attempt.
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.40f, 0.50f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.45f, 0.50f, 50 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Up, 2, 0.45f, 0.50f, 500 * Ms);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 5, 0.45f, 0.50f, 520 * Ms);
    assert(!g.poll(3000 * Ms));
    g.reset();
    // Front three-finger tap: off by default (the rear gesture is the default).
    for (uint64_t f = 1; f <= 3; ++f) g.touch(TouchPanel::Front, TouchPhase::Down, f, 0.2f * f, 0.5f, f * 50 * Ms);
    assert(!g.poll(200 * Ms));
    g.reset();
    g.front = true;
    for (uint64_t f = 1; f <= 3; ++f) g.touch(TouchPanel::Front, TouchPhase::Down, f, 0.2f * f, 0.5f, f * 50 * Ms);
    assert(g.poll(200 * Ms) && !g.poll(300 * Ms));
    g.reset();
    // One or two front fingers (a menu tap): never; three spread over a second: never.
    g.touch(TouchPanel::Front, TouchPhase::Down, 1, 0.5f, 0.5f, 0);
    g.touch(TouchPanel::Front, TouchPhase::Down, 2, 0.6f, 0.5f, 10 * Ms);
    assert(!g.poll(1000 * Ms));
    g.reset();
    g.touch(TouchPanel::Front, TouchPhase::Down, 1, 0.5f, 0.5f, 0);
    g.touch(TouchPanel::Front, TouchPhase::Down, 2, 0.6f, 0.5f, 400 * Ms);
    g.touch(TouchPanel::Front, TouchPhase::Down, 3, 0.7f, 0.5f, 900 * Ms);
    assert(!g.poll(1000 * Ms));
    g.reset();
    // Rear off ("front" setting): the rear hold does nothing.
    g.rear = false;
    g.touch(TouchPanel::Rear, TouchPhase::Down, 1, 0.40f, 0.50f, 0);
    g.touch(TouchPanel::Rear, TouchPhase::Down, 2, 0.45f, 0.50f, 50 * Ms);
    assert(!g.poll(3000 * Ms));
    std::puts("settings_page_test: rear two-finger hold (together, still, 1 s; other fingers ignored), front three-finger "
              "tap, rejections and their counters");
}

std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void storage() {
    using namespace twv::vita;
    // Nothing chosen: "{}"; every value chosen: all four keys; round trip.
    char text[512];
    VitaSettings none;
    assert(format_vita_settings(none, text, sizeof text) && std::string(text) == "{}\n");
    VitaSettings s;
    s.renderScale = 50;
    s.renderScaleSet = true;
    s.batterySaver = true;
    s.performanceSet = true;
    s.showFps = true;
    s.showFpsSet = true;
    s.gesture = OpenGesture::Both;
    s.gestureSet = true;
    assert(format_vita_settings(s, text, sizeof text));
    int problems = -1;
    VitaSettings back = parse_vita_settings(text, &problems);
    assert(problems == 0 && back.renderScale == 50 && back.batterySaver && back.showFps &&
           back.gesture == OpenGesture::Both && back.renderScaleSet && back.performanceSet && back.showFpsSet &&
           back.gestureSet);
    // Only what was chosen is written.
    VitaSettings one;
    one.showFps = false;
    one.showFpsSet = true;
    format_vita_settings(one, text, sizeof text);
    assert(std::strstr(text, "showFps") && !std::strstr(text, "renderScale") && !std::strstr(text, "performance"));
    back = parse_vita_settings(text, &problems);
    assert(problems == 0 && back.showFpsSet && !back.renderScaleSet && back.renderScale == 75 && !back.batterySaver);
    // Bad values are ignored and counted; unknown keys are ignored; other keys still read.
    back = parse_vita_settings("{\"renderScale\": 33, \"performance\": \"turbo\", \"showFps\": 1,"
                               " \"openGesture\": \"rear\", \"future\": {\"renderScale\": 50}}", &problems);
    assert(problems == 3 && !back.renderScaleSet && back.renderScale == 75 && !back.performanceSet &&
           !back.showFpsSet && back.gestureSet && back.gesture == OpenGesture::Rear);
    back = parse_vita_settings("not json", &problems);
    assert(problems == 1 && !back.any_set());
    back = parse_vita_settings("", &problems);
    assert(problems == 0 && !back.any_set());
    // Files: written through the temporary file, replacing an older file; a missing file reads the
    // temporary one (a write cut between remove and rename).
    const auto dir = std::filesystem::temp_directory_path() / ("twv-settings-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "vita_settings.json").string(), temp = (dir / "vita_settings.json.tmp").string();
    VitaSettings got;
    assert(read_vita_settings(path.c_str(), temp.c_str(), got) == 0);
    assert(write_vita_settings(s, path.c_str(), temp.c_str()));
    assert(!std::filesystem::exists(temp));
    assert(read_vita_settings(path.c_str(), temp.c_str(), got, &problems) == 1 && problems == 0 && got.renderScale == 50);
    assert(write_vita_settings(one, path.c_str(), temp.c_str()));
    assert(read_file(path.c_str()).find("renderScale") == std::string::npos);
    std::filesystem::rename(path, temp);
    assert(read_vita_settings(path.c_str(), temp.c_str(), got) == 1 && got.showFpsSet && !got.renderScaleSet);
    std::filesystem::remove(temp);
    std::ofstream(path) << std::string(VitaSettingsLimit + 10, ' ');
    assert(read_vita_settings(path.c_str(), temp.c_str(), got) == -1);
    std::filesystem::remove_all(dir);
    std::puts("settings_page_test: vita_settings.json round trip, chosen keys only, bad values, atomic write");
}

void script() {
    twv::vita::InputScript sc;
    assert(sc.parse("wait 2\npage open\npage DOWN\npage wait 2\npage RIGHT\npage SHOT\npage BACK\nhold A 1\n"));
    // Pad reads: the waits, then "page open" (a read without input).
    const twv::vita::InputStep* step = sc.next();
    assert(step && !step->page && !sc.page_pending() && sc.next() && sc.next()->page == twv::vita::ScriptPageOpen);
    // The step just read was "page open": the page now reads its keys; pad reads leave them alone.
    assert(sc.page_pending() && sc.page_pending());
    assert(sc.page_key() == st::KeyDown && sc.page_key() == twv::vita::ScriptPageWait &&
           sc.page_key() == twv::vita::ScriptPageWait && sc.page_key() == st::KeyRight &&
           sc.page_key() == twv::vita::ScriptPageShot && sc.page_key() == st::KeyBack);
    assert(sc.page_key() == 0 && !sc.page_pending()); // the next step is a pad step: not the page's
    step = sc.next();
    assert(step && step->buttons == twv::vita::ScriptButtonA && !step->page);
    assert(!sc.next());
    // The open and gesture steps belong to pad reads, never to the page.
    twv::vita::InputScript o;
    assert(o.parse("page open\n") && !o.page_pending() && o.page_key() == 0 && o.next()->page == twv::vita::ScriptPageOpen);
    assert(o.parse("page gesture\npage BACK\n") && !o.page_pending() && o.page_key() == 0);
    assert(o.next()->page == twv::vita::ScriptPageGesture && o.page_pending() && o.page_key() == st::KeyBack);
    for (const char* bad : {"page\n", "page down\n", "page OPEN\n", "page UP 2\n", "page JUMP\n", "page wait\n",
                            "page wait 0\n", "page wait x\n"})
        assert(!o.parse(bad));
    std::puts("settings_page_test: input script page steps (open by a pad read, keys by the page)");
}

void drawing(const char* ppm) {
    std::vector<uint32_t> pixels(std::size_t(st::Width) * st::Height, 0);
    std::vector<uint32_t> game(std::size_t(768) * 408);
    for (int y = 0; y < 408; ++y)
        for (int x = 0; x < 768; ++x)
            game[std::size_t(y) * 768 + x] = twv::ui::rgb(uint32_t(x * 255 / 720) & 0xff, uint32_t(y * 255 / 408) & 0xff, 160);
    const twv::ui::Canvas c{pixels.data(), st::Width, st::Height, st::Width};
    st::Page page;
    page.selected = 1;
    page.set_allowed(st::Id::Language, 0x1F, 0);
    st::PageText t;
    t.build = "DuskOnVita 00.01 build 1a2b3c4d (2026-10-28)";
    t.disc = "Disc GZ2P01 (Europe)";
    t.opens = "Settings: hold two fingers on the rear touch pad for 1 second";
    const st::Backdrop backdrop{game.data(), 720, 408, 768};
    st::draw_page(c, page, backdrop, t);
    // The backdrop is the game's frame at a quarter: a corner pixel samples game (0,0).
    const uint32_t g00 = game[0];
    assert(pixels[std::size_t(st::Height - 1) * st::Width] != 0 && pixels[0] == (0xff000000u | ((g00 >> 2) & 0x3f3f3fu)));
    // Every pixel is opaque, and text was drawn in the title, list, help and footer bands.
    for (uint32_t p : pixels) assert((p >> 24) == 0xff);
    const auto lit = [&](int y0, int y1) {
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < st::Width; ++x) n += pixels[std::size_t(y) * st::Width + x] == twv::ui::palette::Text ||
                                                     pixels[std::size_t(y) * st::Width + x] == st::colors::Label ||
                                                     pixels[std::size_t(y) * st::Width + x] == st::colors::Value ||
                                                     pixels[std::size_t(y) * st::Width + x] == twv::ui::palette::Title ||
                                                     pixels[std::size_t(y) * st::Width + x] == st::colors::Help ||
                                                     pixels[std::size_t(y) * st::Width + x] == st::colors::Footer;
        return n;
    };
    assert(lit(st::TitleY, st::TitleY + 30) > 100 && lit(st::ListTop, st::ListTop + st::RowHeight * st::VisibleRows) > 2000);
    assert(lit(st::HelpY, st::HelpY + 40) > 200 && lit(st::FooterY, st::Height) > 300);
    // Without a game frame: a plain backdrop.
    st::draw_page(c, page, st::Backdrop{}, t);
    assert(pixels[0] == st::colors::Dim);
    if (ppm) {
        st::draw_page(c, page, backdrop, t);
        std::FILE* f = std::fopen(ppm, "wb");
        assert(f);
        std::fprintf(f, "P6\n%d %d\n255\n", st::Width, st::Height);
        for (uint32_t p : pixels) {
            const unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8),
                                          static_cast<unsigned char>(p >> 16)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
    std::puts("settings_page_test: page drawn opaque over the dimmed game frame, plain without one");
}
} // namespace

int main(int argc, char** argv) {
    entries();
    navigation();
    actions_and_language();
    taps();
    presets();
    repeat();
    gesture();
    storage();
    script();
    drawing(argc > 1 ? argv[1] : nullptr);
    std::puts("settings_page_test: passed");
    return 0;
}
