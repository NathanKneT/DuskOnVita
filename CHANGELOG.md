# Changelog

Dates are the days the work was checked on a PS Vita 1000. Frame rates are console measurements;
[docs/benchmarks.csv](docs/benchmarks.csv) has the runs.

## 0.9.0 (November 2026)

The first version with a release page. No game data is included: you need your own dump of the
GameCube disc.

Play and presentation
- The map screens draw their map: the dungeon map with its rooms and doors, the field map with its
  terrain and zoom. The game's own off-screen map pass is drawn on the CPU; the minimap in play
  stays off (14 Nov).
- Comfort options on the settings page, Dusklight's own and all off by default: auto-save, instant
  text, hold to mash, quick transform, faster climbing, iron boots, Tears of Light and area
  changes, instant saves, rupee options, bigger wallets, aiming reticle, low-health beep, HUD
  hidden or resized, gyro Rollgoal, aim invert. A Challenge section (damage taken, no heart drops)
  and a Cheats section (infinite supplies) (13 Nov).
- The settings page has sections (L and R, or a tap on the section name) and a "Restart the game"
  entry for the settings that apply at the next start; a screen on the first launch says where the
  settings are (13 Nov).
- The disc image is also looked for in `data/twilight-vita/` on `uma0:`, `imc0:` and `xmc0:`
  (13 Nov).
- The HUD draws Square left of Cross and Circle right, as the buttons sit on the Vita; "HUD button
  positions: Original" in the settings puts the GameCube's places back (12 Nov).
- The HUD's face symbols are thicker and sit on a dark backing, so they read on a bright
  background; SELECT is drawn as the button's oval instead of the word (12 Nov).
- "Free camera" in the settings (Dusklight's own option, off by default); "Camera invert up/down"
  applies to it (11 Nov).
- An icon, a LiveArea background and a gate picture, drawn in code (11 Nov).
- In-game settings page, opened from the rear touch pad or with three fingers on the screen:
  presets, render size, speed-ups, FPS counter, prompts, confirm button, language, gyro, camera
  (5 Nov).
- Disc images as ISO, CISO or GCZ, read in place; the console's language and Enter button are
  followed (5 Nov).
- PlayStation button prompts, touch in menus, gyro aim, start-up screens that say what is missing
  (28 Oct).

Fixes
- A stop at an area change or at the restart after a fall: Link's own heaps were twenty times the
  original's size (Dusklight's room for mods), 13.3 MiB in one block of a 27 MiB heap, and the
  game stops when that block is not free. They are three times the original's size now. Found by
  loading every room of the game in the emulator; nine area changes in a row on the console
  (14 Nov).
- Dusklight's log file is written at the info level instead of debug, so loads write less to the
  memory card; the game's own error lines are kept in the runtime log (14 Nov).
- A crash when entering some areas of Hyrule Field: an enemy could be created one game tick before
  Link and read him. An enemy's creation now waits for the player (12 Nov).
- Saves: rolling backups and a repair check at start (28 Oct).

Speed (all exact: same picture and same game, checked on the console)
- Seven more defaults (13 Nov): draw commands that follow each other with nothing changed between
  them go out as one draw (the Palace of Twilight went from 7.0 to 13.3 FPS, Snowpeak Ruins room 9
  from 8.7 to 18.9); a simple shadow's volume is one joined draw (Ordon village's widest view
  went from 8.7 to 9.9 FPS); the sound is rendered one small step at a time (late sound buffers
  fell from 29 % to 0.2 % in the Palace of Twilight, and to about a quarter in the enemy areas of
  Hyrule Field); repeated register writes skip their handlers; child particles go out ten per
  draw; the room background is not drawn a second time in the presentation walk; the pad values
  come from SDL's events.
- Patient geometry cache; wide geometry cache and block decode; a cheaper pad read (10 and 11 Nov).
  Castle Town went from about 20 to 25.5-27 FPS.
- The GX worker checks less and writes uniforms straight into GPU memory; faster model maths
  (10 Nov). Hyrule Field went from 18 to 28 FPS.
- A "Speed-ups" setting, off by default, that skips work you can't see (6 Nov).
- The game ticks at a fixed 30 Hz and keeps its real speed down to 15 FPS; the picture is drawn at
  75 % and scaled (28 Oct).

Known issues: see the README.
