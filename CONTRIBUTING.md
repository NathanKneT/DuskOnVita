# Contributing

Small, focused pull requests are the easiest to review. Build fixes, reduced reproduction cases and
console measurements are as welcome as new features. The README lists the areas where help is most
wanted; open an issue before a large change so others can avoid duplicate work.

## Ground rules

- **No game data, ever.** Do not attach or commit disc images, files extracted from a disc, textures,
  models, audio, text from the game, firmware, the shader compiler module, saves, shader warm-up
  bundles or screenshots of copyrighted artwork. Describe a scene by its stage, room and spawn instead
  (for example `R_SP01,4,2,0`), and attach receipts.
- **Game behavior stays the original's.** A change to game code goes into `patches/dusklight/` as a
  small diff whose description says what it changes and why it cannot alter what the game does.
- **New renderer paths start as switches.** Compile them behind a build option, select them at
  start-up, keep them off by default, report the choice in the receipts, and document them in
  `docs/PERFORMANCE.md`. A switch becomes the default after console runs show it is correct and faster.
- **Measure on the console.** A performance change needs a before and after run on a Vita with the
  same scene, switches and run length (see `docs/PERFORMANCE.md`). Emulator runs may show that the
  picture and the game state are unchanged, not that something is faster.
- **Keep upstream separate.** Never commit changes inside `external/dusklight`; edit or add a patch in
  `patches/<module>/`. Keep each patch's description at its top, before the diff.

## Workflow

1. Build with `bash tools/vita_build.sh build-vita` (or `bash tools/vita_perf_build.sh` for console
   measurements) and check `build-vita/logs/build.log`.
2. Run `bash tools/host_tests.sh` when you change platform code. When a test compares against a frozen
   baseline in `tests/fixtures/baseline/`, keep the baseline and update the test.
3. Write commit titles that say what changed (`Fix sampler state after texture reuse`, `Handle short
   disc reads`), and explain the problem and how you validated it in the pull request. Say plainly
   when console testing is still pending.

## Code style

Follow the surrounding code. Platform code that includes the VitaSDK headers is built with short
enums; everything else uses int-sized enums. Exchange only plain integer types between the two
(`docs/ARCHITECTURE.md`, "Enum ABI").

By contributing you agree that your contribution is licensed under the MIT license of this
repository, and that patches to upstream files stay under those files' licenses.
