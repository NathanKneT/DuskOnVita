# Roadmap

What's next, roughly in order. It comes from two places: what the console numbers say (README,
"Numbers"), and what people playing the other ports of this game on phones and handhelds ask for
and complain about the most. One rule for everything here: the game has to behave exactly like the
original. Anything that changes the picture, or how random events roll, is a setting, and the
default stays faithful.

## Done on 14 Nov 2026

- The map screens draw their map: dungeon map (rooms, doors) and field map (terrain, zoom), from
  the game's own map pass drawn on the CPU. The minimap in play is still off.
- A crash at area changes fixed: Link's own heaps are three times the original's size instead of
  twenty. Found by loading every room of the game in the emulator.

## Done on 13 Nov 2026

- Dusklight's comfort options on the settings page, each off by default: auto-save, instant text,
  quick transform, the HD version's faster climbing, Tears of Light and bigger wallets, an aiming
  reticle, HUD size and more; a Challenge section and a few cheats.
- Sections on the settings page, a restart entry, a first-launch screen that says where the
  settings are, and the disc image found on a second storage device.

## Done on 12-13 Nov 2026, speed

- Seven more exact speed-ups on by default after console sessions: rows of identical squares as
  one draw (the Palace of Twilight 7 to 13 FPS, Snowpeak Ruins room 9 9 to 19), simple shadows as
  one draw, the sound rendered in small steps (late sound buffers 29 % to 0.2 % in the Palace of
  Twilight), repeated register writes skipped, child particles ten per draw, the room background
  drawn once, and the pad values from SDL's events.
- Measurements for what comes next: which register writes a draw really needs, what ends a run of
  squares, the draw time of the game steps that aren't shown, the sound's time split into the
  game's driver and the mixer.

## Done on 11-12 Nov 2026

- The HUD's button icons sit as the Vita's buttons do, with a setting to put the GameCube's places
  back; readable symbols on any background; SELECT drawn as the button's shape.
- A free camera setting; an icon and LiveArea pictures.
- More exact speed-ups on by default: the patient and wide geometry caches, block decode and a
  cheaper pad read. Castle Town went from about 20 to 25.5-27 FPS.
- A crash fixed when entering some areas of Hyrule Field, found by a tour of 60 places in the
  emulator, which now runs without a fault.
- Version 0.9.0, the first with a release page.

## Done on 6-10 Nov 2026

- The GX worker does less: it no longer checks again a pipeline that didn't change, it no longer
  gets the minimap the renderer can't draw yet, and uniforms go straight into GPU memory.
- Faster model maths: matrix products and weight envelopes through NEON, plus actor searches, hair
  and random numbers, all with the same results bit for bit.
- All of it on by default. On the console, Hyrule Field went from 18 to 28 FPS; Ordon village
  centre and Kakariko gained 1.5 to 2 FPS.
- A Speed-ups setting: Original keeps the game as it was made, Performance skips work you can't
  see. Off by default.

## Done on 5 Nov 2026

- An in-game settings page, opened from the rear touch pad: presets (quality, balanced, battery),
  render size, an FPS counter, button prompts, the confirm button, the language, gyro and camera.
- Your disc image as ISO, CISO or GCZ, read in place; the region read from the disc itself.
- The Vita's language and Enter button picked up automatically; Cross/Circle swap with the prompts
  following.
- Dusklight updated, and an upstream Aurora fix for texture filtering.

## Done on 28 Oct 2026

- The game ticks at a fixed 30 Hz, so it keeps its real speed down to 15 FPS.
- Full-width 16:9, drawn at 75 % and scaled up.
- Start-up screens: progress while shaders get ready, and plain error screens for a missing or
  wrong disc image or a missing shader compiler.
- Saves: rolling backups, a repair check at boot, and importing `.gci` files.
- PlayStation button prompts, front-touch menus, gyro aim.
- Exact fast paths for the heaviest actor work, and a texture pool leak fixed.

## Next up

| # | Goal | Where it's at | Next step |
| --- | --- | --- | --- |
| 1 | A steady 30 FPS in towns and fields | Open Hyrule Field 29 FPS, Castle Town 25.5-27, Snowpeak Ruins 19, Kakariko 18-19, Ordon village 14-16 and about 10 in its widest view, the Palace of Twilight entrance and the field's enemy areas around 14. The main thread is the limit in all of them. The villages come first: 24 to 25 FPS in the worst view is the bar | Find out where the GX worker's backend step spends its time (a console run), then move GXM submission to the third core or keep each material's decoded state; time the part of each game tick that no timer covers yet |
| 2 | No shader stutter anywhere | Rare in the bench places once the warm-up file covers them | A warm-up file for the whole game. How it gets to players is still open, since it's derived from the game's materials |
| 2b | Sound on time everywhere | Late buffers are gone except in the enemy areas of Hyrule Field (about a quarter late: the mixer needs 7 ms of a 10.7 ms buffer with 39 voices) | Make the mixer cheaper, or give the sound thread a core of its own there |
| 3 | Long sessions without crashes | Test runs last minutes, not hours | Track memory use per area, long scripted runs in the emulator, a pass over the spots where other ports crash, then an hour of real play on the console |
| 4 | Your disc, your language | ISO, CISO, GCZ; the region, language and Enter button from the disc and the Vita (done, checked in the emulator) | A USA and a Japanese disc on the console |
| 5 | Settings without editing files | The settings page, presets and Speed-ups are in | The HUD kept sharp while the 3D is scaled; shadow and bloom settings once those effects exist |
| 6 | Minimap, then shadows and bloom | The map screens are drawn (14 Nov, on the CPU); the minimap, shadows and bloom are not: EFB copies aren't in | Copies and off-screen targets on GXM, the minimap first, redrawn only when it changes so the field keeps its speed; then shadows and bloom as settings |
| 7 | Sleep and resume | Not tested | A console checklist: sleep mid-game, in a cutscene and right after saving, and go back and forth to LiveArea; check picture, sound, gyro, touch, speed and the in-game clock |
| 8 | Measure where it actually hurts | Six bench places | Add a horse ride across the field, some grass, a dungeon; count hitches over 100 ms; run the bench before every release |
| 9 | Releases you can trust | First test release with a short install guide | A changelog and the bench table with every release, a short "how to send a crash log" |
| 10 | Battery mode | The Battery preset: 50 % render size and the console's own clocks | Lower clocks while the game is paused |
| 11 | Trophies | Not started. Dusklight already keeps a list of achievements | Real system trophies, one per achievement plus a platinum, for players who install the optional NoTrpDrm plugin; nothing unlocks while a cheat is on |
| 12 | Controls of your own | A fixed layout; the confirm button follows the console or your choice | Z on a tap of its HUD icon, then a few layouts to choose from, then free remapping. A third item slot isn't in Dusklight itself; it comes after remapping, if at all |
| 13 | Mods, texture packs, the randomizer | Not supported | Measure the memory a pack would need on the console first (the game already uses most of what an application gets); data-only packs such as translations before anything else |
| 14 | More disc formats | ISO, CISO and GCZ; RVZ and the others get a screen that says to convert them | Measure what reading RVZ in place would cost in memory and loading time, then decide |
| 15 | Skipping cutscenes | Not in Dusklight itself | See whether a skip can be offered without breaking the flags the story sets during a cutscene |

## Not planned

- 60 FPS, unlocked frame rates or interpolation: the game's logic runs at 30 ticks, and we're not at
  30 yet anyway.
- Dynamic resolution: at 75 % the GPU isn't the problem, the CPU is.
- A touch overlay for gameplay: the Vita has real buttons. Touch stays for menus.
- Draw-distance cuts that change what the game simulates by default. Opt-in settings may pause far,
  unseen ambient creatures (fish, pumpkins, chickens), never anything that quests, items or saves
  depend on.
