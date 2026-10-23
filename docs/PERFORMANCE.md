# Performance: switches, receipts and how to measure

The PS Vita gives an application three of its four Cortex-A9 cores, at up to 444 MHz without plugins.
The original game simulates 30 ticks per second, so a frame has 33 ms. Every change here is judged by
console measurements. The Vita3K emulator is useful to check that a change keeps the picture and the
game state, never to state a frame rate.

## Results so far (PS Vita 1000, console)

All console runs are in [benchmarks.csv](benchmarks.csv) (`python3 tools/charts.py` redraws the
charts), measured by `tools/vita_run_report.py` over steady windows. The latest valid build, v61, at
the system clocks (333 MHz CPU, 111 MHz GPU):

| Scene | FPS | Game ticks/s |
| --- | --- | --- |
| Link's house (`R_SP01`) | 6.34 | 6.14 |
| Ordon village (`F_SP103`) | 7.39 | 5.95 |

Where one 154 ms frame goes in Link's house (v61, everything on one core): game simulation 12 ms, J3D
draw building 20 ms, GX command decode 31 ms, vertex preparation 66 ms, vitaGL calls 25 ms. Build v54
was faster but rejected: it lost draws through uniform failures.

Not yet measured on the console: the 444 MHz default (`--vita-clocks`), static geometry by reference
(`--vita-static-geometry=on`) and the GX worker thread (`--vita-gx-thread=on`). In the emulator, static
geometry in verify mode compared 337,408 certified hits in three scenes with no mismatch, and cut the
bytes compared per 1,020 frames in the house from 3.07 GB to 2.37 MB.

## Two builds

- `bash tools/vita_build.sh build-vita`: the default build. Per-draw diagnostic timers on, the
  optional renderer paths below compiled out.
- `bash tools/vita_perf_build.sh` (`build-vita-perf/`): the measurement build. Every start-up renderer
  path compiled in and off unless selected; per-draw timers, the vertex work probe and the cache
  re-decode oracle compiled out; frame, inclusive and scene receipts and the decode probe
  (`TWV_VITA_DECODE_PROBE`) kept. Use it on the console.

## Run-time files

In `ux0:data/twilight-vita/`:

| File | Content |
| --- | --- |
| `game.iso` | your disc image |
| `args.txt` | options for every launch, whitespace separated, at most 511 bytes, no comments |
| `sequence.txt` | one run per line (`label: options`), one per launch, in order, then `args.txt` again; the position is kept in `sequence_state.txt` (delete it to start over) |
| `shader_warmup.bin` | optional: Cg sources to compile before gameplay (below) |
| `runtime.jsonl` | written by the port: the receipts |
| `gxp_cache/` | written by the GXM backend: compiled shaders, checked before use |

## Start-up switches

Unknown or repeated switches stop the launch with an `args_error` receipt. Everything else is passed
to Dusklight (`--stage=...`, `--cvar ...`).

Available in every build:

| Switch | Default | Effect |
| --- | --- | --- |
| `--vita-clocks=legacy444\|system` | `legacy444` | 444/222/222/166 MHz (CPU/bus/GPU/crossbar) through scePower, no plugin; `system` keeps the system's clocks, as a control |
| `--vita-exit-after=SECONDS` | off | 10-3600: the run ends itself that long after Link first appears, with the receipts written |
| `--vita-static-geometry=off\|on\|verify` | `off` | J3D display lists and their arrays by reference (aurora 0006, dusklight 0014); certified cache hits skip the byte comparison; `verify` compares every certified hit and counts differences |
| `--vita-gx-thread=off\|on` | `off` | Aurora's GX FIFO, and every vitaGL draw, on a worker thread on core 1 (aurora 0007); the game thread keeps core 0 |
| `--vita-gpu=vgl\|gxm` | `vgl` | the renderer: vitaGL, or the experimental direct GXM backend ([ARCHITECTURE.md](ARCHITECTURE.md)); if it fails to start, the run writes `gfx_init_failure` and exits |
| `--vita-gxm-resident=on\|off` | `off` | GXM only: certified geometry and static-list indices stay in a GPU memory arena instead of being copied every frame |
| `--vita-gxm-compact=off\|on\|colors` | `off` | GXM only: integer positions, normals and texture coordinates keep their GX format (exact); `colors` also sends RGBA8 colours as bytes |
| `--vita-gxm-primitives=on\|off` | `off` | GXM only: quads, strips and fans drawn natively, without index lists (aurora 0008) |
| `--vita-gxm-gpu-timing=on\|off` | `off` | GXM diagnostic: per-frame GPU time and what presenting waits for, stamped from the GPU's notifications and the display callback (retail GXM has no GPU timestamps) |
| `--vita-gxm-gpu-split=N` | off | GXM diagnostic, N = 2-8: each frame cut into N scenes of equal draw shares, each with its own GPU time; the extra depth loads and stores keep the picture and add GPU time |
| `--vita-gxm-scale=100\|75\|67\|50` | `100` | GXM diagnostic: render at 720x408, 640x368 or 480x272 and let the display scale up, to test whether a frame is fill-bound; changes the picture |
| `--vita-pipeline-fast=off\|on\|verify` | `off` | fill the GX pipeline config in place and look it up by a compact key, instead of rebuilding, hashing and comparing the whole ~2.7 KB config at every state change; `verify` runs both and counts differences (`gfx_pipeline_fast_*_mismatch`) |
| `--vita-gpu-digest=off\|on\|draws:A-B` | `off` | one `gfx_digest` receipt per frame: a hash of what the frame hands to the renderer; `draws:A-B` also writes each draw's hash for frames A to B to `digest_draws.txt` |
| `--vita-capture=A-B/S` | off | PNG captures of frames A, A+S, ... up to B (at most 48) in `ux0:data/twilight-vita/captures/` |
| `--vita-capture-match=on\|off` | `off` | with the digest on, also capture the first frame whose digest is listed in `capture_match.txt` |

In the performance build, or with the matching `-D` option:

| Switch | Build option | Effect |
| --- | --- | --- |
| `--vita-opt-bundle=on\|off` | `TWV_VITA_RENDER_BUNDLE` | the grouped renderer options: compact texture coordinates, resident geometry, the texture upload path |
| `--vita-compact-st`, `--vita-resident-geometry`, `--vita-texture-upload` `=on\|off` | same | one option of the group, overriding the bundle |
| `--vita-submit-observer=on\|off` | same | diagnostic: snapshots each submission's state (not in the bundle) |
| `--vita-memory-pools=legacy\|ram48\|ram64` | same | vitaGL's RAM/CDRAM split: 32/96, 48/80 or 64/64 MiB |
| `--vita-gx-decode=on\|off` | `TWV_VITA_GX_FAST_DECODE` | fast GX register decode and pipeline info reuse (aurora 0003, 0004) |
| `--vita-lazy-vertex-plan=on\|off` | `TWV_VITA_LAZY_VERTEX_PLAN` | deferred CPU vertex-plan setup |
| `--vita-stream-decode=on\|off` | `TWV_VITA_STREAM_DECODE` | attribute-major vertex packing (experimental) |
| `--vita-small-packets=on\|off` | `TWV_VITA_SMALL_PACKET_CACHE` | bounded exact cache for small direct packets |
| `--vita-uncached-staging=on\|off` | `TWV_VITA_UNCACHED_STAGING` | verified staging in uncached RAM |
| `--vita-neon-compare=on\|off` | `TWV_VITA_NEON_COMPARE` | exact NEON comparisons in the geometry cache |
| `--vita-vertex-cull=on\|off` | `TWV_VITA_VERTEX_CULL` | conservative rejection of draws wholly outside the screen |
| `--vita-sparse-spans=on\|off` | `TWV_VITA_SPARSE_SPANS` | compare only the source intervals the indices reference |
| `--vita-shadow-image-skip=on\|off` | `TWV_VITA_SHADOW_IMAGE_SKIP` | skip CPU work for shadow images the renderer cannot draw yet (dusklight 0013); safe with `--vita-gx-thread=on` |
| `--vita-neon-decode=off\|on\|verify` | `TWV_VITA_NEON_DECODE` | NEON conversion of integer positions, normals and texture coordinates in the stream decoder, bit-identical to the scalar loops; `verify` recomputes every vertex with them and counts differences (`gfx_neon_decode_mismatches`). Not measured on the console |
| `--vita-shader-capture=on\|off` | `TWV_VITA_SHADER_SOURCE_CAPTURE` | record every Cg stage the session compiles, for a warm-up bundle |
| `--vita-benchmark=C1-FIGHT` | `TWV_VITA_BENCHMARK` | a scripted 450-frame fight in Faron (`--stage=F_SP108,6,1,0`, fresh start) |

Every selected switch is written to the receipts at start-up (`*_requested`).

## Shader warm-up

Shaders are generated from the game's GX state and compiled on the console by `libshacccg` at first
use, which stalls for up to several seconds. vitaGL keeps every compiled stage in its cache, so a stage
is slow only once. To move that cost to start-up:

1. Run the scenes you play with `--vita-shader-capture=on` (performance VPK). The generated Cg stages
   go to `ux0:data/twilight-vita/shader_sources/`.
2. Pack them: `python3 tools/vita_shader_warmup.py build --sources DIR --output shader_warmup.bin ...`
   (at most 1024 stages and 4 MiB; `inspect` checks a bundle).
3. Copy `shader_warmup.bin` to `ux0:data/twilight-vita/`. The first launch compiles it before the
   first game frame; later launches load the stages from the cache.

Stages outside the bundle still compile when first met; `gfx_shader_play_compiles` and
`gfx_shader_play_slow_compiles` (50 ms or more) count them after Link first appears. The bundle
encodes the game's material configurations: make it on your console for your own use, and do not
share or commit it.

## Comparing renderers

The draw digest hashes content, never handles or addresses: each program's Cg sources, each texture's
size and uploaded bytes and sampler, each draw's state, the vertex bytes the GPU reads and the
indices. Two runs, or two renderers, fed the same draws give the same digest frame by frame.
`python3 tools/vita_gpu_compare.py digests RUN_A/runtime.jsonl RUN_B/runtime.jsonl` lines two runs up
and reports the first frame that differs; `match-list` writes the digests of a run's captures for
`--vita-capture-match`; `pixels CAPTURES_A CAPTURES_B` compares captures with equal digests pixel by
pixel.

## Receipts

`ux0:data/twilight-vita/runtime.jsonl`, one JSON object per line (`event`, `value` or `text`, `ms`).
Buffered lines are written by a low-priority thread on core 2, and drained at a stop.

- At boot: clocks requested and read, memory, the build's compiled options and every switch.
- Profile windows of 30 frames: frame time mean, maximum, 95th percentile and histogram
  (`prof_frame_*`, `prof_interval_*`), inclusive costs of the game execute, painter, FIFO, shaders,
  present and audio (`prof_*_us`), draws and compiled shader stages (`gfx_*`), shader stages
  prepared by the warm-up and compiled in play (`shader_warmup_*`, `gfx_shader_play_*`).
- Scene probe (performance build): stage, room and whether Link exists, after each FIFO drain.
- Decode probe (performance build): on one frame in 30, the GX worker's FIFO time split exactly into
  kinds of work (`gfx_decode_probe_*`: BP, XF and CP register decode, static lists, draw parsing,
  pipeline lookup, draw setup, backend draws, the rest); `tools/vita_run_report.py` prints it per run.
- Off-screen draws the renderer drops, per target size (`gfx_offscreen_discard_<w>x<h>`).
- GXM GPU timing: GPU busy, latency and idle time, present waits, flip interval and fence waits
  (`gxm_gpu_*`, `gxm_present_*`), and with the split one `gxm_gpu_segment` per scene; the run report
  prints them. Emulator values mean nothing here.
- Stops: `stub_trap` (an unimplemented symbol), `abort`, `terminate`, the end of a timed run.

## Measuring on the console

1. Build the performance VPK. Put one run per line in `sequence.txt` (at most five or six per
   session), each with `--vita-exit-after=` so every run lasts the same time.
2. Keep the scene and the route fixed (`--stage=...`, the player idle or the benchmark fight). Say
   whether the shader cache was cold or warm.
3. Change one thing per run, and repeat the reference run: a difference smaller than twice the
   spread between two reference runs is noise.
4. Summarize with `python3 tools/vita_run_report.py runtime.jsonl`. It counts only steady windows
   (Link present for 10 s, no shader compiled in the window) and prints FPS, ticks per second and the
   inclusive costs per run; `--ledger FILE.csv --version V --commit C` appends one CSV row per run.
5. Post the rows, the commit, the switches and the console model in a performance issue.
