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
//  Receipts particle_batch_* (0056, counting only): every particle emitter draw by outcome
//    (drawn as one batch by Dusklight, or why not) with its number of live particles.
//  --vita-crowd=full|half|low (0048): how many passers Castle Town's crowd managers keep walking.
//    Not exact for the same reason.
//  --vita-fast-particle-palette=on (0059; exact by construction, off by default): child particles,
//    which the game draws one per draw, go out up to ten per draw through the ten position-matrix
//    slots; each vertex is still transformed by its own particle's matrix. Receipt
//    particle_batch_palette_*: draws made that way and the children in them.
//  --vita-fast-present-bg=on (0058; meant to be exact, off by default): the room's background
//    actor does not run its draw a second time in the presentation walk when frame interpolation
//    is off. The per-frame draw digest in fixed-tick runs is the check.
//  --vita-sync-loads=on (test only, 0057): the thread that queues a load request waits until the
//    loader thread has finished it, so loading takes the same number of game ticks in every run
//    and two emulator runs can be compared frame by frame.
//  --vita-state-digest=on (test only): one hash of the game state per tick (every actor's place,
//    angles, speeds, health, status and condition flags; the save data; both random generators),
//    the check that a run with a skip on plays the same game as a run with it off.
#include <cstring>

namespace twv::vita {
enum VisualSkip : int { SkipHiddenDraw = 0, SkipOffscreenAnim = 1, RestCreatures = 2, VisualSkipCount };
constexpr const char* VisualSkipNames[VisualSkipCount] = {"skip-hidden-draw", "skip-offscreen-anim",
                                                          "rest-creatures"};
// patches/dusklight/0056: the outcomes of an emitter's particle draw (twv_vita_particle_batch_note).
constexpr int ParticleBatchReasons = 7;
constexpr const char* ParticleBatchReasonNames[ParticleBatchReasons] = {
    "batched", "other_draw", "projected", "callback", "colour", "children", "palette"};
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
// patches/dusklight/0062: 1 with --vita-state-digest=on. With it, or with the hidden draw skip on,
// the game reports each actor type whose draw method drew random numbers (receipt draw_rnd_types:
// "type:draws" pairs) and the skip keeps such a type on the full path from then on.
extern int twv_draw_rnd_watch;
void twv_vita_draw_rnd_note(int name);
// --vita-place=X,Y,Z,ANGLE (bench lines only, with --develop): once, 30 game ticks after Link
// first exists, he is put at that position facing ANGLE (the game's 16-bit angle), standing still.
// For bench lines at a spot no spawn point gives, such as the widest view of Ordon village
// (console free play of 12 Nov). Receipt place_applied.
void twv_vita_place_select(float x, float y, float z, int angle);
// --vita-fast-particle-palette: 0 off, 1 on.
void twv_vita_fast_particle_palette_select(int on);
extern int twv_fast_particle_palette_mode;
// --vita-fast-present-bg: 0 off, 1 on.
void twv_vita_fast_present_bg_select(int on);
extern int twv_fast_present_bg_mode;
// --vita-sync-loads (test only): 0 off, 1 on; called on the game thread before the game starts.
void twv_vita_sync_loads_select(int on);
extern int twv_sync_loads_mode;
// patches/dusklight/0057 (m_Do_dvd_thread.cpp): returns once *done is set by the loader thread.
void twv_vita_sync_load_wait(const bool* done);
// The game patches, per event: 0 draw skipped, 1 calc skipped, 2 calc replayed, 3 table full,
// 4 an actor rested this tick.
void twv_vita_visual_skip_count(int what);
// patches/dusklight/0056 (JPAResource.cpp), per emitter draw: the outcome (an index into
// ParticleBatchReasonNames) and how many particles the emitter has alive.
void twv_vita_particle_batch_note(int reason, int particles);
// patches/dusklight/0070 (f_op_actor.cpp): an enemy's creation waited a tick for the player.
void twv_enemy_without_player(int name, unsigned id, int waited_ticks);
// benchmark/fixture.cpp's per-tick hook, after every game tick.
void twv_vita_visual_skip_tick(void);
}
