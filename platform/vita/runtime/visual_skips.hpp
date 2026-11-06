#pragma once
// Visual-only speed-ups: the picture may change, the game may not. Off by default; the settings
// page's speed-up entries and these options turn them on.
//  skip-hidden-draw (patches/dusklight/0045): with fixed game ticks a loop may run two ticks before
//    it paints, and the paint shows only the last one. On: in a tick that another tick follows, the
//    actors' draw methods do not run. fopAc_Draw's own work still runs (the culling test and the
//    "not drawn" condition, which execute reads), and types on the keep list draw as before.
//  skip-offscreen-anim (0046): actors of a few ambient types (the pond's fish while no fishing rod
//    is out, birds, frogs) that were culled on their last draw skip their skeleton's model calc in
//    execute; their logic reads no joint, and the calc runs just before their next draw.
//  rest-creatures (0047): the pond's fish (no rod out), pumpkins and chickens out of view and far
//    from Link take the game's own no-execute path; they wake in view, near Link or with a rod out.
//    Not exact: resting actors draw no random numbers, so the shared random sequence changes.
//  --vita-crowd=full|half|low (0048): how many passers Castle Town's crowd managers keep walking.
//    Not exact for the same reason.
//  --vita-state-digest=on (test only): one hash of the game state per tick (every actor's place,
//    angles, speeds, health, status and condition flags; the save data; both random generators),
//    the check that a run with a skip on plays the same game as a run with it off.
#include <cstring>

namespace twv::vita {
enum VisualSkip : int { SkipHiddenDraw = 0, SkipOffscreenAnim = 1, RestCreatures = 2, VisualSkipCount };
constexpr const char* VisualSkipNames[VisualSkipCount] = {"skip-hidden-draw", "skip-offscreen-anim",
                                                          "rest-creatures"};
enum CrowdLevel : int { CrowdFull = 0, CrowdHalf = 1, CrowdLow = 2, CrowdLevelCount };
constexpr const char* CrowdLevelNames[CrowdLevelCount] = {"full", "half", "low"};

// "--vita-crowd=full|half|low": 1 (choice set), -1 for another value or a second time, 0 otherwise.
inline int crowdOption(const char* token, int& choice) {
    constexpr const char Prefix[] = "--vita-crowd=";
    if (std::strncmp(token, Prefix, sizeof Prefix - 1) != 0) return 0;
    if (choice >= 0) return -1;
    for (int level = 0; level < CrowdLevelCount; ++level)
        if (std::strcmp(token + sizeof Prefix - 1, CrowdLevelNames[level]) == 0) {
            choice = level;
            return 1;
        }
    return -1;
}

// "--vita-<name>=off|on" for a skip, or "--vita-state-digest=off|on": 1 (choice set), -1 for another
// value or a second time, 0 for any other token.
inline int visualSkipWord(const char* rest, const char* name, int& choice) {
    const std::size_t n = std::strlen(name);
    if (std::strncmp(rest, name, n) != 0 || rest[n] != '=') return 0;
    if (choice >= 0) return -1;
    if (std::strcmp(rest + n + 1, "off") == 0) choice = 0;
    else if (std::strcmp(rest + n + 1, "on") == 0) choice = 1;
    else return -1;
    return 1;
}
inline int visualSkipOption(const char* token, int (&choices)[VisualSkipCount], int& digest) {
    constexpr const char Prefix[] = "--vita-";
    if (std::strncmp(token, Prefix, sizeof Prefix - 1) != 0) return 0;
    const char* rest = token + sizeof Prefix - 1;
    for (int which = 0; which < VisualSkipCount; ++which)
        if (const int r = visualSkipWord(rest, VisualSkipNames[which], choices[which])) return r;
    return visualSkipWord(rest, "state-digest", digest);
}

struct VisualSkipStats {
    // draws skipped on hidden ticks; model calcs skipped in execute; skipped calcs run again before
    // a draw; calcs not skipped because the replay table was full; resting actor ticks.
    unsigned long long draws = 0, calcs = 0, replays = 0, full = 0, rests = 0;
};
VisualSkipStats visualSkipStats();
}

extern "C" {
// Read by the game patches: 0 off, 1 on.
extern int twv_skip_hidden_draw_mode;
extern int twv_skip_offscreen_anim_mode;
extern int twv_rest_creatures_mode;
extern int twv_crowd_level;
// One more at the end of every game tick (the game patches tell a tick's own records apart).
extern unsigned twv_vita_tick_serial;
// main_vita.cpp, the settings page: 0 off, 1 on. Returns 0 for any other value or switch.
int twv_vita_visual_skip_select(int which, int on);
int twv_vita_crowd_select(int level);
void twv_vita_state_digest_select(int on);
// The game patches, per event: 0 draw skipped, 1 calc skipped, 2 calc replayed, 3 table full,
// 4 an actor rested this tick.
void twv_vita_visual_skip_count(int what);
// benchmark/fixture.cpp's per-tick hook, after every game tick.
void twv_vita_visual_skip_tick(void);
}
