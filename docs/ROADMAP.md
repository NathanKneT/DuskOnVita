# Roadmap

What's next, roughly in order. It comes from two places: what the console numbers say (README,
"Numbers"), and what people playing the other ports of this game on phones and handhelds ask for
and complain about the most. One rule for everything here: the game has to behave exactly like the
original. Anything that changes the picture is a setting, and the default stays faithful.

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
| 1 | A steady 30 FPS in towns and fields | 12-28 FPS in the six bench places; the main thread is the bottleneck in five of them | Exact speed-ups for the actors at the top of the console ranking (fish, the town crowd, a few enemies, Link) and for the painter; the GX worker where it's the limit |
| 2 | No shader stutter anywhere | Rare in the bench places once the warm-up file covers them | A warm-up file for the whole game. How it gets to players is still open, since it's derived from the game's materials |
| 3 | Long sessions without crashes | Test runs last minutes, not hours | Track memory use per area, long scripted runs in the emulator, a pass over the spots where other ports crash, then an hour of real play on the console |
| 4 | Your disc, your language | ISO, CISO, GCZ; the region, language and Enter button from the disc and the Vita (done, checked in the emulator) | A USA and a Japanese disc on the console |
| 5 | Settings without editing files | The settings page and presets are in (5 Nov) | The HUD kept sharp while the 3D is scaled; shadow and bloom settings once those effects exist |
| 6 | Minimap and map, then shadows and bloom | Not drawn yet: EFB copies aren't in | Copies and off-screen targets on GXM, minimap and map first (in progress), then shadows and bloom as settings |
| 7 | Sleep and resume | Not tested | A console checklist: sleep mid-game, in a cutscene and right after saving, and go back and forth to LiveArea; check picture, sound, gyro, touch, speed and the in-game clock |
| 8 | Measure where it actually hurts | Six bench places | Add a horse ride across the field, some grass, a dungeon; count hitches over 100 ms; run the bench before every release |
| 9 | Releases you can trust | First test release with a short install guide | A changelog and the bench table with every release, a short "how to send a crash log" |
| 10 | Battery mode | The Battery preset: 50 % render size and the console's own clocks | Lower clocks while the game is paused |

## Not planned

- 60 FPS, unlocked frame rates or interpolation: the game's logic runs at 30 ticks, and we're not at
  30 yet anyway.
- Dynamic resolution: at 75 % the GPU isn't the problem, the CPU is.
- A touch overlay for gameplay: the Vita has real buttons. Touch stays for menus.
- Draw-distance cuts that change what the game simulates. At most an opt-in setting that skips
  drawing far things, never the game's own updates.
