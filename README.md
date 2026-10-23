# DuskOnVita

An independent, unofficial project that ports **The Legend of Zelda: Twilight Princess** to the
PlayStation Vita.

- It is not affiliated with or endorsed by Nintendo, Sony, the Dusklight team or the zeldaret project.
- It is built on [Dusklight](https://github.com/TwilitRealm/dusklight), the native build of the
  [Twilight Princess decompilation](https://github.com/zeldaret/tp); the name refers to Dusklight.
- It contains no game data. You need your own legally obtained copy of the game.

The original game code runs natively on the Vita's ARM CPU. This repository adds the Vita platform
layer (a renderer that translates Aurora's GX commands to
[vitaGL](https://github.com/Rinnegatamante/vitaGL), or experimentally straight to GXM, audio, input,
memory, receipts) and a small queue of reviewed patches to the pinned upstreams.

**It is not playable yet.** The aim is a stable 30 FPS; the console runs the game at a fraction of
that today. The code is shared so others can reproduce the work and help.

## Status (measured on a PS Vita 1000)

Frames per second, build g2c at 444 MHz:

| Scene | vitaGL | GXM + G2 |
| --- | ---: | ---: |
| Link's house | 20.2 | 19.4 |
| Ordon village | 22.9 | 27.2 |
| Faron fight | 11.7 | 14.0 |

The best result, 27.2 FPS, is Ordon village on GXM. Link's house runs at about 20 FPS on either
renderer, and the Faron fight at 12 to 14. vitaGL is the default renderer. GXM + G2 is the experimental native renderer (`--vita-gpu=gxm`) with
resident geometry, packed vertex formats and native primitives. Both columns use static geometry by
reference, the GX worker thread, the fast pipeline lookup and a shader warm-up bundle; the switches are
in [docs/PERFORMANCE.md](docs/PERFORMANCE.md). For comparison, build v61 (one core, system clocks) ran
at 6.3 FPS in the house and 7.4 in Ordon.

Full speed is 30. Every figure comes from a console run, averaged over steady windows (Link in the
scene for 10 s, no shader compiling; `tools/vita_run_report.py`); emulator numbers are only used to
check correctness. All console runs are listed in [docs/benchmarks.csv](docs/benchmarks.csv);
`python3 tools/charts.py` redraws the charts from it.

![PS Vita frame rate, build by build](docs/img/progress.svg)

In the house, the GX worker needs about 37 ms per frame with vitaGL and 31 ms with GXM. With GXM the
CPU work fits in a 30 FPS frame (33 ms), but frames still take about 50 ms; the GPU timing switches
(`--vita-gxm-gpu-timing`, see [docs/PERFORMANCE.md](docs/PERFORMANCE.md)) are there to find out why.
Outdoors in Ordon, GXM is CPU-bound and about 3 ms short of 30 FPS. Where the GX worker's time goes
in the house with GXM:

![Where the GX worker's time goes in Link's house](docs/img/frame.svg)

## What works

| Area | State | Notes |
| --- | --- | --- |
| Link's house, Ordon village, the Faron fight (development starts) | ✅ | started with `--stage` arguments |
| Native GXM renderer | ⚠️ | experimental: `--vita-gpu=gxm`; vitaGL stays the default |
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

- **Graphics translation cost:** the GX worker needs 31 to 37 ms per frame in the house (see the chart
  above); after the draws themselves, pipeline lookup and register decode are the largest parts.
  Profiles and patches welcome.
- **GPU time on GXM:** the house takes about 50 ms per frame although its CPU work fits in 33 ms;
  console runs with `--vita-gxm-gpu-timing` will show where the time goes.
- **Frame-buffer effects:** EFB copies and off-screen passes, on vitaGL and on GXM.
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
