# Roadmap

What's next, roughly in order. It comes from two places: what the console numbers say (README,
"Numbers"), and what people playing the other ports of this game on phones and handhelds ask for
and complain about the most. One rule for everything here: the game has to behave exactly like the
original. Anything that changes the picture is a setting, and the default stays faithful.

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
| 1 | A steady 30 FPS in towns and fields | 12-28 FPS in the six bench places; the main thread is the bottleneck in five of them | Exact speed-ups for the actors at the top of the console ranking (fish, the town crowd, a few enemies, Link) and for the painter; the GX worker where it's the limit |
| 2 | No shader stutter anywhere | Rare in the bench places once the warm-up file covers them | A warm-up file for the whole game. How it gets to players is still open, since it's derived from the game's materials |
| 3 | Long sessions without crashes | Test runs last minutes, not hours | Track memory use per area, long scripted runs in the emulator, a pass over the spots where other ports crash, then an hour of real play on the console |
| 4 | Your disc, your language | European ISO only | CISO images, region read from the disc itself, the Vita's language picked up automatically, Cross/Circle swap with the prompts following (in progress) |
| 5 | Settings without editing files | `args.txt` and Dusklight's config only | An in-game settings page, render presets (battery, balanced, quality), the HUD kept sharp while the 3D is scaled (in progress) |
| 6 | Minimap and map, then shadows and bloom | Not drawn yet: EFB copies aren't in | Copies and off-screen targets on GXM, minimap and map first (in progress), then shadows and bloom as settings |
| 7 | Sleep and resume | Not tested | A console checklist: sleep mid-game, in a cutscene and right after saving, and go back and forth to LiveArea; check picture, sound, gyro, touch, speed and the in-game clock |
| 8 | Measure where it actually hurts | Six bench places | Add a horse ride across the field, some grass, a dungeon; count hitches over 100 ms; run the bench before every release |
| 9 | Releases you can trust | Source only | Tagged releases with a changelog and the bench table, a three-step install guide, a short "how to send a crash log" |
| 10 | Battery mode | 444 MHz, set by the app itself, no plugin | A preset with lower clocks, 50 % render size and effects off; lower clocks while the game is paused |

## Not planned

- 60 FPS, unlocked frame rates or interpolation: the game's logic runs at 30 ticks, and we're not at
  30 yet anyway.
- Dynamic resolution: at 75 % the GPU isn't the problem, the CPU is.
- A touch overlay for gameplay: the Vita has real buttons. Touch stays for menus.
- Draw-distance cuts that change what the game simulates. At most an opt-in setting that skips
  drawing far things, never the game's own updates.
