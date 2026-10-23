# Architecture

The port builds [Dusklight](https://github.com/TwilitRealm/dusklight) for the PS Vita. Dusklight is the
zeldaret/tp decompilation turned into a native program on top of
[Aurora](https://github.com/encounter/aurora), a reimplementation of the GameCube SDK (GX, VI, PAD, OS,
DVD, CARD), and [Borealis](https://github.com/encounter/borealis) (logging, files, settings).

```
 game + JSystem + Z2AudioLib   decompiled game code, built as is (int-sized enums)
 src/dusk                      Dusklight's platform layer: settings, the software DSP mixer, frame
                               pacing; RmlUi/ImGui menus, mods and networking compiled out
 Borealis                      log, io, data, cli, task; net reports "unavailable"
 Aurora                        OS, VI, PAD, SI, CARD, THP, DVD; GX frontend -> FIFO -> command processor
 platform/vita/gfx_vgl         Aurora's gfx contract on vitaGL (GXM): Cg shaders, vertices, textures
 platform/vita                 entry point, clocks, receipts, audio output, input defaults, run control
 VitaSDK, vitaGL, SDL3         system libraries, the GL layer over GXM, SDL's Vita input and events
```

## Rules

- **Behavior comes from the decompilation.** Game code (`src/d`, `src/f_*`, `src/m_Do`,
  `libs/JSystem`, Z2AudioLib) is built from Dusklight unchanged except for the patches in
  `patches/dusklight/`. Each patch starts with what it changes and why it cannot change what the game
  does, or names the switch that turns it on.
- **Upstream stays upstream.** Dusklight (with Aurora and Borealis as its submodules) is a pinned
  submodule; the other sources, vitaGL included, are archives pinned by SHA-256 in `deps.lock`.
  `tools/vita_build.sh` applies `patches/` to a mirror of the tree, never to the submodules.
- **Missing pieces fail visibly.** The link runs twice: a probe link that allows unresolved symbols,
  then `tools/gen_stubs.py` turns every one into a named trap stub, and the final link is strict.
  Reaching a stub stops the game and writes the symbol to the receipts; `link_report.txt` lists them.
- **Optional paths are switches.** Every renderer optimization that is not yet proven on the console
  is compiled behind a build option and selected at start-up, off by default, and reported in the
  receipts ([PERFORMANCE.md](PERFORMANCE.md)).
- **No game data in the repository.** Everyone supplies their own disc image and shader compiler.

## Repository layout

| Path | Content |
| --- | --- |
| `platform/vita/` | entry point (`main_vita.cpp`), `runtime/` (receipts, clocks, run control, GX thread, probes), `gfx_vgl/` (renderer), `audio/`, `aurora/` (Aurora's application glue), `disc/` (disc reader), `benchmark/` (opt-in fixture), `compat/`, `probes/` |
| `patches/<module>/` | diffs applied to the build mirror: `dusklight`, `aurora`, `borealis`, `deps/<name>` |
| `cmake/` | upstream file lists, dependencies, vitaGL, platform libraries, the VPK link |
| `tools/` | build scripts, dependency fetcher, stub generator, enum-ABI audit, receipt reports, test fixtures, charts |
| `tests/` | host tests; `tests/fixtures/baseline/` holds frozen earlier versions of this port's own files that some tests compare against |

Some files are shared with the port's Nintendo 3DS counterpart (the disc reader, Aurora glue, the null
gfx, the receipt format) and keep the same paths.

## Enum ABI

`arm-vita-eabi` uses short enums by default (an enum whose values fit in a byte is one byte). The
decompilation assumes int-sized enums, as on the GameCube, and lays such types over file data, so
everything built from source here uses `-fno-short-enums`. Only the platform files that include the
VitaSDK headers keep the toolchain default, because the prebuilt SDK libraries (newlib, libstdc++,
SDL3, the psp2 stubs) use it. Rules:

1. No enum-typed value crosses that boundary: the interfaces between the two sides use plain integers.
2. SDK out-parameters are word-sized buffers or structs of pinned size, checked with
   `static_assert` (a data abort on a Vita 1000 came from breaking this).
3. `tools/enum_abi_audit.py` runs on every build and reports struct layout differences in the SDL3 and
   libstdc++ headers, and which sources touch them.

## Memory

The newlib heap is 160 MiB (`_newlib_heap_size_user`, `main_vita.cpp`); GPU memory comes from vitaGL's
own pools, split between RAM and CDRAM by `--vita-memory-pools`. The game's heaps are bounded for a
handheld (patch `dusklight/0009`): 64 MiB MEM1 and 16 MiB ARAM, twice the original archive and J2D
heaps and six times the original game heap, instead of Dusklight's desktop reserves. These are initial
capacities, not a validated full-game budget.

## Threads

- **Core 0:** the game thread: game logic, J3D building GX commands, and by default (Inline mode,
  patch `aurora/0001`) also Aurora's GX processing and every vitaGL call.
- **Core 1:** with `--vita-gx-thread=on` (patch `aurora/0007`), a worker processes the GX FIFO and so
  runs every draw. The game still waits for it at the end of its drawing (GXDrawDone), so only J3D's
  command building overlaps the GX work.
- **Core 2:** the audio worker (SceAudioOut) and the receipt writer.

Only one thread uses vitaGL at a time: textures, EFB copies and draws are made inside FIFO processing;
the game thread reaches the renderer only between frames.

## Rendering

The renderer (`platform/vita/gfx_vgl`) implements the contract Aurora's GX unit tests use: the GX
frontend, FIFO, command processor and register state run unchanged and hand finished draws to it.

- **Shaders:** Cg programs generated from the GX TEV and vertex state, compiled on the console by
  `libshacccg` and cached; an optional warm-up bundle moves first-use compilation to start-up.
- **Vertices:** transforms, lighting and ordinary texgen run in the vertex shader. The CPU decodes the
  GX attributes (big-endian, indexed per attribute, fixed point) into vertex buffers and caches the
  result in a bounded cache that compares the source bytes before reuse. With
  `--vita-static-geometry=on`, lists and arrays that the game provably did not change skip that
  comparison (patches `aurora/0006`, `dusklight/0014`).
- **Textures:** converted from GX formats and uploaded through vitaGL, with a conversion cache.
- **Not done yet:** off-screen passes and EFB copies (shadows, heat haze, the minimap) and some
  indirect-texture effects. `--vita-shadow-image-skip` skips the CPU work of the shadow images the
  renderer cannot draw.

## Audio

Dusklight emulates the GameCube DSP in software at 48 kHz. Its output goes to SceAudioOut through a
worker thread that owns the port (patch `dusklight/0003`, `platform/vita/audio`) instead of an SDL
audio stream, so the audio thread's core, priority and suspend behavior belong to the port. What the
game renders is unchanged; surround modes render stereo.

## Files

- `ux0:data/twilight-vita/`: `game.iso`, `args.txt`, `sequence.txt`, `shader_warmup.bin`,
  `runtime.jsonl`, and Dusklight's user directory (`--user-dir`).
- `ux0:data/TwilitRealm/Dusklight/`: Dusklight's logs and caches.
- `ur0:data/libshacccg.suprx`: the shader compiler, extracted on your own console.
- The disc reader (`platform/vita/disc/gcm_nod.cpp`) implements the part of the nod API that Aurora
  and Borealis call, for plain ISO/GCM images only. The VPK carries no Dusklight resource files.
