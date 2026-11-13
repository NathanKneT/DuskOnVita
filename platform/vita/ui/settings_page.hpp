#pragma once
// The settings page: its entries, values and navigation (no drawing, no SDK; ui/settings_draw.hpp
// draws it, ui/settings_screen.cpp runs it on the Vita). Host-tested in tests/settings_page_test.cpp.
//
// The sections are listed on the left; the right side shows the entries of the chosen section only,
// so the list scrolls inside a section and never across the whole page. D-pad up/down chooses an
// entry (past a section's end it goes on into the next section, wrapping at the ends), left/right
// changes its value (wrapping), the confirm button changes it too or runs an action, triangle puts
// the entry back to its default, back or START closes the page. L and R jump to the previous and
// the next section. Front-screen taps: a section on the left opens it, a tap on an entry chooses it,
// a tap on its value (left or right half) changes it, a tap on an action runs it, a tap on the
// footer's "Close" closes.
//
// The Comfort, Challenge and Cheats entries are Dusklight's own options (dusk/settings.h): each is
// off by default, and off is the game as it was made.
// Values are choice indices; the page shell maps them to Dusklight settings and vita_settings.json.
#include <cstdint>
#include <cstring>

namespace twv::ui::settings {

enum class Id : uint8_t {
    Preset, RenderScale, Performance, SpeedUps, SkipUnseen, StillAnimals, RestingAnimals, Crowd, ShowFps, ButtonPrompts, HudButtons, ConfirmButton, Language, GyroAim, GyroSensitivity,
    GyroInvertYaw, GyroInvertPitch, GyroRecentre, GyroRollgoal, FreeCamera, CameraInvertX, CameraInvertY, AimInvertX,
    AimInvertY,
    AutoSave, InstantText, HoldToMash, QuickTransform, FastClimbing, FastIronBoots, FastTears, FastTransitions,
    InstantSaves, RupeeMessages, RupeesAlwaysFit, BiggerWallets, AimingReticle, LowHealthBeep, HideHud, HudSize,
    DamageTaken, HeartDrops,
    InfiniteHearts, InfiniteArrows, InfiniteBombs, InfiniteRupees, InfiniteOil, InfiniteAir,
    OpenGesture, RestartGame, ResetDefaults, Close, Count
};
constexpr int EntryCount = int(Id::Count);
enum class Kind : uint8_t { Choice, Action };

// Sections, each named by its first entry: L and R jump between them, the title shows the current one.
constexpr int SectionCount = 8;
constexpr Id SectionFirst[SectionCount] = {Id::Preset, Id::ButtonPrompts, Id::GyroAim, Id::FreeCamera,
                                           Id::AutoSave, Id::DamageTaken, Id::InfiniteHearts, Id::OpenGesture};
constexpr const char* SectionNames[SectionCount] = {"Display & speed", "Buttons & text", "Gyro",
                                                    "Camera & aim", "Comfort", "Challenge", "Cheats",
                                                    "This page"};
// One line under a section's name, for the sections that change the game.
constexpr const char* SectionNotes[SectionCount] = {
    "", "", "", "", "Off is the game as it was made.", "The defaults are the game as it was made.",
    "Off is the game as it was made.", ""};
constexpr int section_of(Id id) {
    int section = 0;
    for (int k = 1; k < SectionCount; ++k)
        if (int(id) >= int(SectionFirst[k])) section = k;
    return section;
}
// When a change takes effect (shown next to the help line).
enum class Applies : uint8_t { Now, NextArea, Restart };

struct Entry {
    Id id;
    Kind kind;
    const char* label;
    const char* help; // one or two short lines (wrapped when drawn)
    Applies applies;
    const char* const* choices;
    uint8_t count;
    uint8_t defaultIndex;
};

// Choice values the shell maps (settings_screen.cpp): the order of each list is part of the mapping.
constexpr int RenderScaleValues[] = {50, 67, 75, 100};
constexpr int GyroSensitivityHundredths[] = {25, 50, 75, 100, 125, 150, 200, 250, 300};
constexpr int LanguageValues[] = {0, 1, 2, 3, 4, 6}; // Dusklight GameLanguage (OS_LANGUAGE_*; Japanese 6)
constexpr int HudSizeHundredths[] = {75, 100, 125, 150}; // game.hudScale
constexpr int DamageTakenValues[] = {1, 2, 4};            // game.damageMultiplier
// Presets set Render size and Performance together (choice indices of those two entries). "Custom"
// (PresetCustom) is shown when the two match no preset; it is never chosen by a key.
struct PresetChoice { int renderScale, performance; };
constexpr PresetChoice Presets[] = {{3, 0}, {2, 0}, {0, 1}}; // Quality 100 %, Balanced 75 %, Battery 50 % + saver
constexpr int PresetCount = 3, PresetCustom = 3;
// Speed-ups set the four entries after them together (runtime/visual_skips.hpp): Original keeps
// the game as it was made; Performance turns on every speed-up, the crowd at half. "Custom"
// (SpeedCustom) is shown when the four match neither; it is never chosen by a key.
struct SpeedChoice { int skipUnseen, stillAnimals, restingAnimals, crowd; };
constexpr SpeedChoice SpeedPresets[] = {{0, 0, 0, 0}, {1, 1, 1, 1}};
constexpr int SpeedPresetCount = 2, SpeedCustom = 2;

namespace text {
constexpr const char* Preset[] = {"Quality", "Balanced", "Battery", "Custom"};
constexpr const char* RenderScale[] = {"50 %", "67 %", "75 %", "100 %"};
constexpr const char* Performance[] = {"Normal 444 MHz", "Battery saver"};
constexpr const char* SpeedUps[] = {"Original", "Performance", "Custom"};
constexpr const char* Crowd[] = {"Full", "Half", "Low"};
constexpr const char* OffOn[] = {"Off", "On"};
constexpr const char* Prompts[] = {"PlayStation", "GameCube"};
constexpr const char* HudButtons[] = {"Vita pad", "Original"};
constexpr const char* Confirm[] = {"System", "Cross", "Circle"};
constexpr const char* Language[] = {"English", "German", "French", "Spanish", "Italian", "Japanese"};
constexpr const char* Sensitivity[] = {"0.25", "0.50", "0.75", "1.00", "1.25", "1.50", "2.00", "2.50", "3.00"};
constexpr const char* Gesture[] = {"Rear: 2 fingers", "Front: 3 fingers", "Rear or front"};
constexpr const char* HudSize[] = {"75 %", "100 %", "125 %", "150 %"};
constexpr const char* Damage[] = {"Original", "Double", "Four times"};
} // namespace text

constexpr Entry Entries[EntryCount] = {
    {Id::Preset, Kind::Choice, "Preset",
     "Quality: 100 %, 444 MHz. Balanced: 75 %, 444 MHz. Battery: 50 %, the console's own clocks. "
     "Sets the next two entries.", Applies::Restart, text::Preset, 4, 1},
    {Id::RenderScale, Kind::Choice, "Render size",
     "Smaller is faster, larger is sharper. The HUD is drawn at the same size.", Applies::Restart,
     text::RenderScale, 4, 2},
    {Id::Performance, Kind::Choice, "Performance",
     "Normal: 444 MHz, the highest clock the Vita lets a game use (no plugin, no overclock). "
     "Battery saver: the console's own clocks (333 MHz CPU); slower game, longer battery.",
     Applies::Restart, text::Performance, 2, 0},
    {Id::SpeedUps, Kind::Choice, "Speed-ups",
     "Original: the game as it was made. Performance: the four entries below on (crowd at half); "
     "quests, items and saves are not affected.", Applies::Now, text::SpeedUps, 3, 0},
    {Id::SkipUnseen, Kind::Choice, "Skip unseen frames",
     "Below 30 FPS the game runs two steps per frame and shows only the second: the first is not drawn. "
     "The game plays the same.", Applies::Now, text::OffOn, 2, 0},
    {Id::StillAnimals, Kind::Choice, "Still animals off screen",
     "Fish, birds and frogs out of view stop moving their bodies until you see them again. "
     "The game plays the same.", Applies::Now, text::OffOn, 2, 0},
    {Id::RestingAnimals, Kind::Choice, "Resting animals far away",
     "Fish, pumpkins and chickens far from Link and out of view pause until he comes near. "
     "Random events then roll differently.", Applies::Now, text::OffOn, 2, 0},
    {Id::Crowd, Kind::Choice, "Castle Town crowd",
     "How many passers walk the streets; fewer is faster. Random events then roll differently.",
     Applies::NextArea, text::Crowd, 3, 0},
    {Id::ShowFps, Kind::Choice, "Show FPS", "Frames per second in the top-left corner.", Applies::Now,
     text::OffOn, 2, 0},
    {Id::ButtonPrompts, Kind::Choice, "Button prompts",
     "Button icons in the HUD and in texts. HUD icons change at the next door or area.", Applies::NextArea,
     text::Prompts, 2, 0},
    {Id::HudButtons, Kind::Choice, "HUD button positions",
     "Vita pad: Square left of Cross and Circle right, as on the console. Original: the GameCube's "
     "places (also used when Circle confirms). Changes at the next door or area.", Applies::NextArea,
     text::HudButtons, 2, 0},
    {Id::ConfirmButton, Kind::Choice, "Confirm button",
     "The button that confirms (A). System follows the Vita's own setting.", Applies::Restart, text::Confirm,
     3, 0},
    {Id::Language, Kind::Choice, "Language", "Text language, from the languages on your disc.",
     Applies::Restart, text::Language, 6, 0},
    {Id::GyroAim, Kind::Choice, "Gyro aim", "Aim the bow and other first-person items by tilting the Vita.",
     Applies::Now, text::OffOn, 2, 1},
    {Id::GyroSensitivity, Kind::Choice, "Gyro sensitivity", "How far the aim turns when you tilt the Vita.",
     Applies::Now, text::Sensitivity, 9, 3},
    {Id::GyroInvertYaw, Kind::Choice, "Gyro invert left/right", "Turn the gyro aim the other way sideways.",
     Applies::Now, text::OffOn, 2, 0},
    {Id::GyroInvertPitch, Kind::Choice, "Gyro invert up/down", "Turn the gyro aim the other way up and down.",
     Applies::Now, text::OffOn, 2, 0},
    {Id::GyroRecentre, Kind::Action, "Recentre gyro",
     "Forget the held angle: the way you hold the Vita when the game resumes is the new neutral.",
     Applies::Now, nullptr, 0, 0},
    {Id::GyroRollgoal, Kind::Choice, "Gyro Rollgoal",
     "Tilt the Vita to play Rollgoal in the fishing hole's cabin. Off: the stick, as the original.",
     Applies::Now, text::OffOn, 2, 0},
    {Id::FreeCamera, Kind::Choice, "Free camera",
     "Off: the original camera (the right stick turns it sideways; up looks through Link's eyes). "
     "On: the right stick moves the camera freely, up and down too.", Applies::Now, text::OffOn, 2, 0},
    {Id::CameraInvertX, Kind::Choice, "Camera invert left/right", "Right stick camera, sideways.", Applies::Now,
     text::OffOn, 2, 0},
    {Id::CameraInvertY, Kind::Choice, "Camera invert up/down",
     "Right stick camera, up and down. Only the free camera moves up and down.", Applies::Now,
     text::OffOn, 2, 0},
    {Id::AimInvertX, Kind::Choice, "Aim invert left/right",
     "The stick while aiming an item or looking through Link's eyes, sideways.", Applies::Now, text::OffOn, 2,
     0},
    {Id::AimInvertY, Kind::Choice, "Aim invert up/down",
     "The stick while aiming an item or looking through Link's eyes, up and down.", Applies::Now, text::OffOn,
     2, 0},
    // Comfort: Dusklight's options. Off is the original game.
    {Id::AutoSave, Kind::Choice, "Auto-save",
     "Saves into your current file when Link enters a new area or opens a dungeon door. "
     "Off: the game saves only when you ask, as the original.", Applies::Now, text::OffOn, 2, 0},
    {Id::InstantText, Kind::Choice, "Instant text",
     "Hold the cancel button and a text box shows all of its text at once.", Applies::Now, text::OffOn, 2, 0},
    {Id::HoldToMash, Kind::Choice, "Hold to mash",
     "Where the game asks you to press a button again and again, holding it is enough.", Applies::Now,
     text::OffOn, 2, 0},
    {Id::QuickTransform, Kind::Choice, "Quick transform",
     "R + Triangle changes between Link and the wolf without asking Midna, once the story lets you change.",
     Applies::Now, text::OffOn, 2, 0},
    {Id::FastClimbing, Kind::Choice, "Faster climbing",
     "Ladders and vines are climbed faster, as in the HD version.", Applies::Now, text::OffOn, 2, 0},
    {Id::FastIronBoots, Kind::Choice, "Faster iron boots",
     "Link walks faster with the iron boots on.", Applies::Now, text::OffOn, 2, 0},
    {Id::FastTears, Kind::Choice, "Faster Tears of Light",
     "The tear a shadow insect leaves comes out sooner, as in the HD version.", Applies::Now, text::OffOn, 2, 0},
    {Id::FastTransitions, Kind::Choice, "Faster area changes",
     "Shorter fades when Link goes from one map to the next.", Applies::Now, text::OffOn, 2, 0},
    {Id::InstantSaves, Kind::Choice, "Instant saves",
     "No waiting screen while the game writes your save.", Applies::Now, text::OffOn, 2, 0},
    {Id::RupeeMessages, Kind::Choice, "Skip rupee messages",
     "A rupee's message plays only the first time you pick that kind up, not again after each load.",
     Applies::Now, text::OffOn, 2, 0},
    {Id::RupeesAlwaysFit, Kind::Choice, "Always take rupees",
     "Rupees from a chest are taken even when the wallet is full, instead of going back into the chest.",
     Applies::Now, text::OffOn, 2, 0},
    {Id::BiggerWallets, Kind::Choice, "Bigger wallets",
     "The three wallets hold 500, 1000 and 2000 rupees, as in the HD version.", Applies::Now, text::OffOn, 2, 0},
    {Id::AimingReticle, Kind::Choice, "Aiming reticle",
     "A mark shows where the bow and the slingshot aim.", Applies::Now, text::OffOn, 2, 0},
    {Id::LowHealthBeep, Kind::Choice, "Mute low health beep",
     "No beeping while Link is low on hearts.", Applies::Now, text::OffOn, 2, 0},
    {Id::HideHud, Kind::Choice, "Hide the HUD",
     "Hearts, buttons, rupees and the rest of the main HUD are not drawn.", Applies::Now, text::OffOn, 2, 0},
    {Id::HudSize, Kind::Choice, "HUD size",
     "Size of the hearts, the buttons and the other HUD parts. Menus and text boxes keep their size.",
     Applies::Now, text::HudSize, 4, 1},
    // Challenge: Dusklight's options. The defaults are the original game.
    {Id::DamageTaken, Kind::Choice, "Damage taken",
     "How much each hit hurts Link. Double is what the HD version's Hero Mode does.", Applies::Now,
     text::Damage, 3, 0},
    {Id::HeartDrops, Kind::Choice, "No heart drops",
     "Enemies, pots and grass never drop hearts, as in the HD version's Hero Mode.", Applies::Now,
     text::OffOn, 2, 0},
    // Cheats: Dusklight's options. Off is the original game.
    {Id::InfiniteHearts, Kind::Choice, "Infinite hearts", "Link's hearts stay full.", Applies::Now, text::OffOn,
     2, 0},
    {Id::InfiniteArrows, Kind::Choice, "Infinite arrows", "The quiver stays full.", Applies::Now, text::OffOn, 2,
     0},
    {Id::InfiniteBombs, Kind::Choice, "Infinite bombs", "The bomb bags stay full.", Applies::Now, text::OffOn, 2,
     0},
    {Id::InfiniteRupees, Kind::Choice, "Infinite rupees", "The wallet stays full.", Applies::Now, text::OffOn, 2,
     0},
    {Id::InfiniteOil, Kind::Choice, "Infinite lantern oil", "The lantern never runs out of oil.", Applies::Now,
     text::OffOn, 2, 0},
    {Id::InfiniteAir, Kind::Choice, "Infinite air", "Link never runs out of air under water.", Applies::Now,
     text::OffOn, 2, 0},
    {Id::OpenGesture, Kind::Choice, "Open this page with",
     "Rear: hold two fingers together on the rear touch pad for 1 second. Front: touch the screen with three "
     "fingers.",
     Applies::Now, text::Gesture, 3, 2},
    {Id::RestartGame, Kind::Action, "Restart the game",
     "Closes the game and starts it again, so that entries marked \"next start\" apply. "
     "Progress since your last save is lost.", Applies::Now, nullptr, 0, 0},
    {Id::ResetDefaults, Kind::Action, "Reset to defaults", "Every entry on this page back to its default.",
     Applies::Now, nullptr, 0, 0},
    {Id::Close, Kind::Action, "Close and resume", "Back to the game. Changes are saved now.", Applies::Now,
     nullptr, 0, 0},
};
static_assert(EntryCount == 52);
static_assert(SectionFirst[0] == Id::Preset, "the first section starts at the first entry");
static_assert(Presets[1].renderScale == 2 && Presets[1].performance == 0, "Balanced is the default size and clock");
constexpr const Entry& entry(Id id) { return Entries[int(id)]; }

// --- layout (960x544), shared by the drawing and the touch hit test ---
constexpr int Width = 960, Height = 544;
// Left: the title, the section list, the build and the disc.
constexpr int SidebarW = 232, TitleX = 24, TitleY = 20;
constexpr int SectionTop = 78, SectionHeight = 38, SectionX = 12, SectionW = SidebarW - 24;
constexpr int SidebarInfoY = 438; // "L / R", then the build and the disc
// Right: the section's name, its entries, the help of the chosen one, the controls.
constexpr int ContentX = 256, ContentRight = 936, HeadingY = 22;
constexpr int ListTop = 70, RowHeight = 36, VisibleRows = 9;
constexpr int LabelX = ContentX + 20, ValueX = 652, ValueRight = ContentRight - 16; // the value's column
constexpr int HelpY = ListTop + RowHeight * VisibleRows + 12, HelpLines = 3, HelpLineHeight = 20;
constexpr int FooterY = 486, FooterLine2Y = 514; // the controls; how the page opens
constexpr int CloseButtonX = 820, CloseButtonY = 478, CloseButtonW = 116, CloseButtonH = 34;

// Keys (the shell maps the Vita buttons or a test script onto them).
enum Key : uint32_t {
    KeyUp = 1u << 0, KeyDown = 1u << 1, KeyLeft = 1u << 2, KeyRight = 1u << 3, KeyConfirm = 1u << 4,
    KeyBack = 1u << 5, KeyDefault = 1u << 6, KeyStart = 1u << 7, KeySectionPrev = 1u << 8, KeySectionNext = 1u << 9
};

struct Effect {
    enum Type : uint8_t { None, Changed, Recentre, Close, Restart } type = None;
    Id id = Id::Count;
    int index = 0; // Changed: the new choice index
};
struct Effects {
    static constexpr int Max = EntryCount + 2;
    Effect list[Max];
    int count = 0;
    void add(Effect e) {
        if (count < Max) list[count++] = e;
    }
    bool has(Effect::Type t) const {
        for (int i = 0; i < count; ++i)
            if (list[i].type == t) return true;
        return false;
    }
};

class Page {
public:
    int index[EntryCount] = {};
    int defaults[EntryCount] = {};
    bool available[EntryCount] = {};   // false: not shown (e.g. no FPS counter in this renderer)
    uint32_t allowedChoices[EntryCount] = {}; // bit i: choice i can be chosen (Language: on this disc)
    int selected = 0, top = 0;
    bool resetArmed = false, restartArmed = false;
    const char* status = nullptr; // a short line after an action ("Gyro recentred.")

    Page() {
        for (int i = 0; i < EntryCount; ++i) {
            index[i] = defaults[i] = Entries[i].defaultIndex;
            available[i] = true;
            allowedChoices[i] = Entries[i].count ? (1u << Entries[i].count) - 1u : 0u;
        }
        allowedChoices[int(Id::Preset)] = (1u << PresetCount) - 1u; // Custom is shown, never chosen
        allowedChoices[int(Id::SpeedUps)] = (1u << SpeedPresetCount) - 1u;
    }

    // The speed-up preset that matches its four entries, or Custom (derived, no change emitted).
    void sync_speed() {
        int match = SpeedCustom;
        for (int k = 0; k < SpeedPresetCount; ++k)
            if (index[int(Id::SkipUnseen)] == SpeedPresets[k].skipUnseen &&
                index[int(Id::StillAnimals)] == SpeedPresets[k].stillAnimals &&
                index[int(Id::RestingAnimals)] == SpeedPresets[k].restingAnimals &&
                index[int(Id::Crowd)] == SpeedPresets[k].crowd)
                match = k;
        index[int(Id::SpeedUps)] = match;
    }

    // The preset that matches Render size and Performance, or Custom (no change emitted: derived).
    void sync_preset() {
        int match = PresetCustom;
        for (int k = 0; k < PresetCount; ++k)
            if (index[int(Id::RenderScale)] == Presets[k].renderScale &&
                index[int(Id::Performance)] == Presets[k].performance)
                match = k;
        index[int(Id::Preset)] = match;
    }

    // Choices of an entry; its default when the current value is not allowed.
    void set_allowed(Id id, uint32_t mask, int defaultIndex) {
        const int i = int(id);
        allowedChoices[i] = mask & ((1u << Entries[i].count) - 1u);
        defaults[i] = defaultIndex;
        if (!allowed(id, index[i])) index[i] = defaultIndex;
    }
    bool allowed(Id id, int choice) const {
        return choice >= 0 && choice < Entries[int(id)].count && (allowedChoices[int(id)] >> choice & 1u);
    }
    int choice_count(Id id) const {
        int n = 0;
        for (int c = 0; c < Entries[int(id)].count; ++c) n += allowed(id, c);
        return n;
    }
    const Entry& current() const { return Entries[selected]; }

    // One step of value `v` among the allowed choices of `id`, wrapping.
    int step(Id id, int v, int direction) const {
        const int n = Entries[int(id)].count;
        if (n == 0 || choice_count(id) == 0) return v;
        int next = v;
        for (int k = 0; k < n; ++k) {
            next = ((next + direction) % n + n) % n;
            if (allowed(id, next)) return next;
        }
        return v;
    }

    Effects handle(uint32_t pressed) {
        Effects out;
        if (!pressed) return out;
        const Entry& e = current();
        if (!(pressed & KeyConfirm) || e.id != Id::ResetDefaults) resetArmed = false;
        if (!(pressed & KeyConfirm) || e.id != Id::RestartGame) restartArmed = false;
        if (pressed & (KeyBack | KeyStart)) {
            out.add({Effect::Close, Id::Close, 0});
            return out;
        }
        if (pressed & KeySectionPrev) jump(-1);
        if (pressed & KeySectionNext) jump(+1);
        if (pressed & KeyUp) move(-1);
        if (pressed & KeyDown) move(+1);
        const Entry& now = current();
        if (now.kind == Kind::Choice) {
            if (pressed & KeyLeft) change(now.id, step(now.id, index[selected], -1), out);
            if (pressed & (KeyRight | KeyConfirm)) change(now.id, step(now.id, index[selected], +1), out);
            if (pressed & KeyDefault) change(now.id, defaults[selected], out);
        } else if (pressed & KeyConfirm) {
            activate(now.id, out);
        }
        return out;
    }

    // A front-screen tap at (x, y) in screen pixels.
    Effects tap(int x, int y) {
        Effects out;
        if (x >= CloseButtonX && x < CloseButtonX + CloseButtonW && y >= CloseButtonY &&
            y < CloseButtonY + CloseButtonH) {
            out.add({Effect::Close, Id::Close, 0});
            return out;
        }
        if (x < SidebarW) {
            const int k = y >= SectionTop ? (y - SectionTop) / SectionHeight : -1;
            if (k >= 0 && k < SectionCount && k != section()) {
                resetArmed = restartArmed = false;
                open_section(k);
            }
            return out;
        }
        const int row = row_at(y);
        if (row < 0) return out;
        if (row != selected) resetArmed = restartArmed = false;
        selected = row;
        const Entry& e = Entries[row];
        if (e.kind == Kind::Action) {
            activate(e.id, out); // Reset: the first tap asks, the second resets
        } else if (x >= ValueX) {
            change(e.id, step(e.id, index[row], x < (ValueX + ValueRight) / 2 ? -1 : +1), out);
        }
        return out;
    }

    // The entry drawn at screen row y, or -1.
    int row_at(int y) const {
        if (y < ListTop || y >= ListTop + VisibleRows * RowHeight) return -1;
        int rows[VisibleRows];
        const int n = visible(rows), row = (y - ListTop) / RowHeight;
        return row < n ? rows[row] : -1;
    }
    // The first entry of section k and the one after its last.
    static constexpr int section_begin(int k) { return int(SectionFirst[k]); }
    static constexpr int section_end(int k) { return k + 1 < SectionCount ? int(SectionFirst[k + 1]) : EntryCount; }
    // Entries of the chosen section shown from `top`, in order (for drawing): fills rows[], returns
    // the count.
    int visible(int* rows) const {
        int n = 0;
        const int end = section_end(section());
        for (int i = top; i < end && n < VisibleRows; ++i)
            if (available[i]) rows[n++] = i;
        return n;
    }
    // Shown entries of the chosen section: how many there are, and how many are above `top`.
    int section_rows(int* above = nullptr) const {
        int n = 0, before = 0;
        const int k = section();
        for (int i = section_begin(k); i < section_end(k); ++i) {
            if (!available[i]) continue;
            ++n;
            before += i < top;
        }
        if (above) *above = before;
        return n;
    }
    bool section_shown(int k) const {
        for (int i = section_begin(k); i < section_end(k); ++i)
            if (available[i]) return true;
        return false;
    }
    bool more_above() const {
        int above = 0;
        section_rows(&above);
        return above > 0;
    }
    bool more_below() const {
        int above = 0;
        return section_rows(&above) - above > VisibleRows;
    }
    // The page as it was left: entry `entry` chosen, the list from `first` when that still shows it.
    void reopen(int entry, int first) {
        selected = entry;
        top = first;
        keep_selection_shown();
        int rows[VisibleRows];
        const int n = visible(rows);
        bool shown = false;
        for (int r = 0; r < n; ++r) shown |= rows[r] == selected;
        if (!shown) top = selected;
    }
    // Opens section k at its first shown entry (nothing happens for a section without one).
    void open_section(int k) {
        for (int i = section_begin(k); i < section_end(k); ++i) {
            if (!available[i]) continue;
            selected = top = i;
            status = nullptr;
            return;
        }
    }

    // The section of the chosen entry.
    int section() const { return section_of(Entries[selected].id); }

    // Every entry back to its default (the Reset action); emits a change for each that moved.
    void reset_all(Effects& out) {
        for (int i = 0; i < EntryCount; ++i)
            if (Entries[i].kind == Kind::Choice && Entries[i].id != Id::Preset && Entries[i].id != Id::SpeedUps &&
                available[i] && index[i] != defaults[i])
                change(Entries[i].id, defaults[i], out);
    }

private:
    void move(int direction) {
        for (int k = 0; k < EntryCount; ++k) {
            selected = (selected + direction + EntryCount) % EntryCount;
            if (available[selected]) break;
        }
        keep_selection_shown();
    }
    // `top` stays inside the chosen entry's section, and the chosen entry stays on screen: the list
    // moves only when the selection passes its first or last row.
    void keep_selection_shown() {
        const int k = section();
        if (top < section_begin(k) || top >= section_end(k) || selected < top) {
            top = selected < top && top < section_end(k) && top >= section_begin(k) ? selected : section_begin(k);
        }
        int rows[VisibleRows];
        for (int guard = 0; guard < EntryCount; ++guard) {
            const int n = visible(rows);
            bool shown = false;
            for (int r = 0; r < n; ++r) shown |= rows[r] == selected;
            if (shown || top >= selected) break;
            ++top;
        }
    }
    // To the first shown entry of the previous or next section that has one (wrapping).
    void jump(int direction) {
        const int from = section();
        for (int k = 1; k <= SectionCount; ++k) {
            const int target = ((from + direction * k) % SectionCount + SectionCount) % SectionCount;
            if (!section_shown(target)) continue;
            open_section(target);
            return;
        }
    }
    void change(Id id, int choice, Effects& out) {
        const int i = int(id);
        if (!allowed(id, choice) || choice == index[i]) return;
        index[i] = choice;
        status = nullptr;
        out.add({Effect::Changed, id, choice});
        if (id == Id::Preset) {
            change(Id::RenderScale, Presets[choice].renderScale, out);
            change(Id::Performance, Presets[choice].performance, out);
        }
        if (id == Id::RenderScale || id == Id::Performance) sync_preset();
        if (id == Id::SpeedUps) {
            change(Id::SkipUnseen, SpeedPresets[choice].skipUnseen, out);
            change(Id::StillAnimals, SpeedPresets[choice].stillAnimals, out);
            change(Id::RestingAnimals, SpeedPresets[choice].restingAnimals, out);
            change(Id::Crowd, SpeedPresets[choice].crowd, out);
        }
        if (id == Id::SkipUnseen || id == Id::StillAnimals || id == Id::RestingAnimals || id == Id::Crowd) sync_speed();
    }
    void activate(Id id, Effects& out) {
        switch (id) {
        case Id::GyroRecentre:
            out.add({Effect::Recentre, id, 0});
            status = "Gyro recentred: the angle when the game resumes is neutral.";
            break;
        case Id::ResetDefaults:
            if (!resetArmed) {
                resetArmed = true;
                status = "Press again to put every entry back to its default.";
            } else {
                resetArmed = false;
                reset_all(out);
                status = "Every entry is back to its default.";
            }
            break;
        case Id::RestartGame:
            if (!restartArmed) {
                restartArmed = true;
                status = "Press again to restart. Progress since your last save is lost.";
            } else {
                restartArmed = false;
                out.add({Effect::Restart, id, 0});
            }
            break;
        case Id::Close: out.add({Effect::Close, id, 0}); break;
        default: break;
        }
    }
};

// Held-key repeat for the D-pad: a press, then after 400 ms every 120 ms. Returns the keys that
// count as pressed this frame.
struct KeyRepeat {
    static constexpr uint32_t Repeating = KeyUp | KeyDown | KeyLeft | KeyRight;
    uint32_t last = 0;
    uint64_t since = 0, next = 0;
    uint32_t update(uint32_t held, uint64_t nowUs) {
        const uint32_t fresh = held & ~last;
        uint32_t pressed = fresh;
        const uint32_t repeat = held & Repeating;
        if (fresh & Repeating) {
            since = nowUs;
            next = nowUs + 400000;
        } else if (repeat && nowUs >= next) {
            pressed |= repeat;
            next = nowUs + 120000;
        }
        last = held;
        return pressed;
    }
};

} // namespace twv::ui::settings
