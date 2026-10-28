# Roadmap

What comes next, in priority order. The list comes from console measurements (README, "Status")
and from what players of this game's other ports, on phones and handhelds, ask for and complain
about most. Game behavior stays the original's in every item; anything that changes the picture is a
setting, off or faithful by default.

## Done on 28 Oct 2026

- Game ticks at a fixed 30 Hz: the game keeps its real speed down to 15 FPS.
- Full-width 16:9 picture, drawn at 75 % and scaled up.
- Start-up screens: progress while shaders are prepared, plain errors for a missing or wrong disc
  image and a missing shader compiler.
- Saves: rolling backups, a repair check at boot, import of `.gci` files.
- PlayStation button prompts, front-touch menus, gyro aim.
- Exact fast paths for the busiest actor work, and a texture pool leak fixed.

## Next

| # | Goal | Where it stands | Next step |
| --- | --- | --- | --- |
| 1 | Steady 30 FPS in towns and fields | 12-28 FPS in the six bench places; the main thread is the limit in five | Exact fast paths for the actor types at the top of the console ranking (fish, the town crowd, some enemies, Link) and the painter; the GX worker where it limits |
| 2 | No shader freezes anywhere | Rare in the bench places once the warm-up bundle covers them | A bundle that covers the whole game; how it reaches players is still open, since it is derived from the game's materials |
| 3 | Long play without crashes | Runs last minutes, not hours | Memory high-water receipts per area, long scripted runs in the emulator, a sweep of the scenes where other ports crash, then an hour of console play |
| 4 | Your disc, your language | Plain ISO of the European disc only | CISO images, region read from the disc header, the Vita's language mapped to the disc's languages, Cross/Circle swap with the prompts following (in progress) |
| 5 | Settings without editing files | `args.txt` and Dusklight's config only | An in-game settings page; render presets (battery, balanced, quality); the HUD drawn at full resolution while the 3D scene is scaled (in progress) |
| 6 | Minimap and map screens, then shadows and bloom | Not drawn: EFB copies are not implemented | Copies and off-screen targets on GXM, minimap and map first (in progress), then shadows and bloom as settings |
| 7 | Sleep and resume | Not tested | A console checklist: sleep during play, a cutscene and right after a save; back from LiveArea; check picture, sound, gyro, touch, frame rate and the game clock |
| 8 | Measure where players suffer | Six bench places | Add a horse ride across the field, grass, a dungeon; count hitches over 100 ms; run the bench before every release |
| 9 | Releases you can trust | Source only | Tagged releases with a changelog and the bench table, a three-step install guide, a short guide to sending a crash log |
| 10 | Battery mode | 444 MHz set by the app, no plugin | A preset with lower clocks, 50 % render size and effects off; lower clocks while the game is paused |

## Not planned

- 60 FPS, unlocked frame rates or interpolation: the game's logic is tied to 30 ticks, and the
  console is still short of 30.
- Dynamic resolution: the GPU is not the limit at 75 %; the CPU is.
- A touch overlay for gameplay: the Vita has buttons; touch stays for menus.
- Draw-distance cuts that change what the game simulates: at most an opt-in setting that skips
  drawing, never the game's own updates.
