# DuskOnVita

An independent, unofficial project that ports **The Legend of Zelda: Twilight Princess** to the
PlayStation Vita.

- It is not affiliated with or endorsed by Nintendo, Sony, the Dusklight team or the zeldaret project.
- It is built on [Dusklight](https://github.com/TwilitRealm/dusklight), the native build of the
  [Twilight Princess decompilation](https://github.com/zeldaret/tp); the name refers to Dusklight.
- It contains no game data. You need your own legally obtained copy of the game.

The original game code runs natively on the Vita's ARM CPU. This repository adds the Vita platform
layer (a renderer that translates Aurora's GX commands to
[vitaGL](https://github.com/Rinnegatamante/vitaGL), audio, input, memory, receipts) and a small queue
of reviewed patches to the pinned upstreams.

**It is not playable yet.** The aim is a stable 30 FPS; the console runs the game at a fraction of
that today. The code is shared so others can reproduce the work and help.

## Status (measured on a PS Vita 1000)

| Scene | Latest console result | Notes |
| --- | --- | --- |
| Link's house | **6.3 FPS** | build v61, system clocks (333 MHz), every draw rendered |
| Ordon village, outside | **7.4 FPS** | same build |

Full speed is 30. Every figure comes from a console run, averaged over steady windows (Link in the
scene for 10 s, no shader compiling; `tools/vita_run_report.py`); emulator numbers are only used to
check correctness. All console runs are listed in [docs/benchmarks.csv](docs/benchmarks.csv);
`python3 tools/charts.py` redraws the charts from it.

![PS Vita frame rate, build by build](docs/img/progress.svg)

The original game simulation takes about 12 ms of a 154 ms frame. Most of the frame is spent
translating the game's GameCube graphics commands for vitaGL, all on one core:

![Where one frame goes in Link's house](docs/img/frame.svg)

This code already contains the next steps: the standard 444 MHz clock by default, static models
taken by reference instead of compared byte by byte every frame (`--vita-static-geometry`), and the
graphics translation on its own core (`--vita-gx-thread`). They are checked on the host and in an
emulator; their console results are not measured yet and will be added here.

## What works

| Area | State | Notes |
| --- | --- | --- |
| Link's house and Ordon village (development start) | ✅ | started with `--stage` arguments |
| 3D scenes (textured, lit, fog) | ⚠️ | some textures flicker |
| Controls (both analog sticks, buttons) | ✅ | no touch or gyro |
| Shader preparation | ⚠️ | shaders compile at first use, causing long stalls; an optional warm-up file moves this to startup |
| 3D lock-on marker | ⚠️ | drawn in the wrong place |
| Outdoor scenes | ⚠️ | a black shape appears in some views |
| Frame-buffer effects (shadows, heat haze, minimap) | ❌ | EFB copies and off-screen passes are not implemented yet |
| Saves | ⚠️ | the crash when saving is fixed in this code (`patches/aurora/0005-vita-copy-stream-stack.patch`), not yet confirmed on the console |
| Full opening, dungeons, complete playthrough | ❔ | untested |

## Requirements

- A homebrew-enabled PS Vita with VitaShell.
- Your own GameCube disc image as a plain `.iso`/`.gcm` (RVZ, WIA, CISO and other containers are not
  read). Development and testing use the European release **GZ2P01, revision 0**; other releases are
  untested on this port.
- The shader compiler module `ur0:data/libshacccg.suprx`, extracted on your own console.

## Build

Use Linux or WSL2 with Docker, Git, Bash, Python 3, rsync, patch, curl and unzip. Keep the checkout on
the Linux side (for example `~/src/duskonvita`), so file modes and LF line endings stay intact.

```sh
git submodule update --init external/dusklight
git -C external/dusklight submodule update --init extern/aurora extern/borealis
docker pull vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb
bash tools/vita_build.sh build-vita
```

`tools/vita_build.sh` mirrors the repository into a work tree (`TWV_WORK`, default
`~/twilight-vita-work`), applies `patches/` there (the submodules stay untouched), downloads the
hash-pinned sources in `deps.lock`, and builds in the digest-pinned VitaSDK image with networking off.
It downloads no game data. The result is `build-vita/twilight_vita.vpk`; `link_report.txt` next to it
lists the symbols still resolved by trap stubs.

For measurements, build the performance VPK instead: `bash tools/vita_perf_build.sh` compiles every
start-up renderer switch in (each off unless selected) and leaves out the per-draw diagnostics
([docs/PERFORMANCE.md](docs/PERFORMANCE.md)).

## Run

1. Install the VPK with VitaShell.
2. Copy your disc image to `ux0:data/twilight-vita/game.iso`.
3. Copy `args.example` to `ux0:data/twilight-vita/args.txt` (it starts in Link's house).
4. Launch **DuskOnVita (dev)**.

`args.txt` holds whitespace-separated options, at most 511 bytes, without quoting or comments. Stage
syntax: `--stage=STAGE,ROOM,SPAWN,LAYER`. `sequence.txt` in the same folder runs a list of
configurations, one per launch. Each run appends receipts to `ux0:data/twilight-vita/runtime.jsonl`;
Dusklight's own logs and caches are in `ux0:data/TwilitRealm/Dusklight/`. Keep backups of your saves
outside these folders.

## Controls

| Vita | GameCube |
| --- | --- |
| Left stick / right stick | control stick / C stick |
| Cross / Circle | A / B |
| Square / Triangle | X / Y |
| L / R | L / R (targeting on L) |
| Select | Z |
| Start, D-pad | Start, D-pad |

Button prompts still show the GameCube labels.

## How to measure

Performance claims need a console run. [docs/PERFORMANCE.md](docs/PERFORMANCE.md) lists every switch
and receipt and the protocol: one change per run, the same scene, a fixed run length
(`--vita-exit-after`), loading and shader compilation separated from steady play, and
`tools/vita_run_report.py` to turn `runtime.jsonl` into one row per run.

## Tests

`bash tools/host_tests.sh` builds and runs the host test suite (Linux or WSL, a C++20 compiler with
AddressSanitizer and UndefinedBehaviorSanitizer, Python 3; about 15 minutes). It needs the
submodules: several tests extract the real functions from the pinned upstream sources and from this
port's code, and compare them with frozen earlier versions kept in `tests/fixtures/baseline/`. Host
tests do not prove GPU output or console performance.

## Help wanted

- **Graphics translation cost:** command decode and vertex preparation take about 100 ms of the frame
  (see the chart above). Profiles and patches welcome.
- **Frame-buffer effects:** EFB copies and off-screen passes on vitaGL.
- **Texture flicker:** find the cause; a matched Dolphin capture of the same scene helps.
- **Hardware testers:** a Vita 1000 or 2000, the scene, the build and the `runtime.jsonl` receipts.

[CONTRIBUTING.md](CONTRIBUTING.md) explains how to propose a change.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) describes how the port is put together.

## Credits and license

This port exists thanks to the [zeldaret/tp](https://github.com/zeldaret/tp) decompilation,
[Dusklight](https://github.com/TwilitRealm/dusklight), [Aurora](https://github.com/encounter/aurora),
[Borealis](https://github.com/encounter/borealis), [VitaSDK](https://vitasdk.org) and
[vitaGL](https://github.com/Rinnegatamante/vitaGL). [NOTICE](NOTICE) lists every upstream with its
pinned version and license.

Original code in this repository is available under the [MIT license](LICENSE). Upstream projects and
dependencies keep their own licenses; a patch does not change the license of the file it applies to.
vitaGL is LGPL-3.0-or-later: its pinned source, the local patches and the build recipe are here so it
can be rebuilt and relinked.

The Legend of Zelda and Twilight Princess are trademarks of Nintendo. This repository contains no game
disc image, extracted game data, Nintendo or Sony software, or shader compiler binary.
