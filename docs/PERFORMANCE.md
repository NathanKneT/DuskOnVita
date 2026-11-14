# Performance: switches, receipts and how to measure

The PS Vita gives an application three of its four Cortex-A9 cores, at up to 444 MHz without plugins.
The original game simulates 30 ticks per second, so a frame has 33 ms. Every change here is judged by
console measurements. The Vita3K emulator is useful to check that a change keeps the picture and the
game state, never to state a frame rate.

## Results so far (PS Vita 1000, console)

The latest results (the heavy places on 13 Nov, the lighter ones on 10 and 11 Nov, all with the
play profile) are in the README.
The single runs before them are in [benchmarks.csv](benchmarks.csv) (`python3 tools/charts.py`
redraws the charts), measured by `tools/vita_run_report.py` over steady windows. Build v61, at the
system clocks (333 MHz CPU, 111 MHz GPU):

| Scene | FPS | Game ticks/s |
| --- | --- | --- |
| Link's house (`R_SP01`) | 6.34 | 6.14 |
| Ordon village (`F_SP103`) | 7.39 | 5.95 |

Where one 154 ms frame goes in Link's house (v61, everything on one core): game simulation 12 ms, J3D
draw building 20 ms, GX command decode 31 ms, vertex preparation 66 ms, vitaGL calls 25 ms. Build v54
was faster but rejected: it lost draws through uniform failures.

Those two rows are from before the 444 MHz default (`--vita-clocks`), static geometry by reference
(`--vita-static-geometry=on`) and the GX worker thread (`--vita-gx-thread=on`). All three are part of
the play profile now, and the console numbers in the README are measured with them. In the emulator, static
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

## Play profile

A launch whose command line chooses no renderer option and no GPU backend or feature gets the play
profile: the paths that console runs found correct and faster, as one line (`platform/vita/main_vita.cpp`,
`PlayProfile`): GXM with resident geometry, compact formats and native primitives, the GX worker,
static geometry, the fast pipeline lookup, the performance build's decode and staging options and 64/64
MiB memory pools. With it come fixed 30 Hz ticks, the 75 % render size, frame overlap, fast real
shadows, the fast actor paths marked "play" below and quiet receipts. Diagnostics (digest, captures,
GPU timing, shader capture, input scripts) keep the profile; any renderer option turns it off, and
`--vita-profile=off` keeps every path off. The receipts say which applied (`play_profile`).

## Start-up switches

Unknown or repeated switches stop the launch with an `args_error` receipt. Everything else is passed
to Dusklight (`--stage=...`, `--cvar ...`).

Added on 28 Oct:

| Switch | Default | Effect |
| --- | --- | --- |
| `--vita-profile=play\|off` | play | the play profile above; `play` forces it, and is refused together with renderer options |
| `--vita-ticks=fixed\|frames\|single` | `fixed` with the profile | game ticks at a fixed 30 Hz, at most two per frame (dusklight 0015), so the game keeps its speed down to 15 FPS; `frames` is one tick per frame (as before); `single` is at most one per loop, for emulator A/B runs |
| `--vita-frame-overlap=off\|on\|verify` | `on` with the profile | the first game tick runs while the GX worker finishes the last frame (aurora 0009, dusklight 0019); needs the GX worker. Console, Ordon at 75 %: 20.5 FPS off, 25.9 on. `verify` checks the memory the last frame read against the next tick |
| `--vita-fast-shadow-collect=off\|on\|verify` | `on` with the profile | the ground polygons under real shadows: the walk stops once the 256-triangle list is full and skips prisms the shadow test would reject, with the same list (dusklight 0018); `verify` runs both and reports lists that differ |
| `--vita-fast-<name>=off\|on\|verify` | see right | exact actor and painter fast paths: `search` (0032), `bg-list` (0033, terrain queries) and `flow-bind` (0034, message flows) are on with the profile; `obstacle` (0031) is off; `gx-wake` and `static-dl` (aurora 0020, 0021) are on with the profile since 10 Nov. `verify` runs the original next to the fast path and counts differences |
| `--vita-receipts=full\|quiet\|brief` | `quiet` with the profile | `full` adds the per-actor-type and painter timing used by `tools/vita_bench_report.py --actors --painter`; `quiet` keeps one 30-frame window of timings in every 300 frames; `brief` is quiet with a window every 90 frames, for hand-played console runs |
| `--vita-input-script=FILE` | off | replays a button script from the data folder (`tools/input_scripts/`), for repeatable walks |
| `--vita-import-saves=auto\|off` | `auto` | save import at boot ([ARCHITECTURE.md](ARCHITECTURE.md), "Saves") |
| `--vita-gxm-uniform-split`, `--vita-gxm-fragment-split` `=on\|off\|verify`; `--vita-gxm-uniform-reuse`, `--vita-gxm-pipeline-memo` `=on\|off` | `off` | GXM experiments: lighting and TEV registers in their own uniform buffers, reuse of the last uniform reservation, a memo of the last pipeline checked. The memo is on with the profile since 10 Nov; the others are not measured on the console |

Added on 6 and 10 Nov (every `verify` mode runs the original next to the fast path and counts
differences; in the six bench places in Vita3K they found none):

| Switch | Default | Effect |
| --- | --- | --- |
| `--vita-fast-mtx=off\|on\|verify` | `on` with the profile | `C_MTXConcat` and the J3D weight-envelope sums through NEON, same result bit for bit (aurora 0022, dusklight 0037). Console verify: 10.6 million products, no difference |
| `--vita-fast-search-index`, `--vita-fast-hair`, `--vita-fast-rnd`, `--vita-fast-envelopes` `=off\|on\|verify` | `on` with the profile since 10 Nov | actor ID and name searches from an index of the kept process list (dusklight 0039); the E_s1 hair keeps only the product column it uses (0041); the game's random numbers without `fmodf` (0040); a weight envelope multiplies each joint's matrices once per calc instead of once per mix (0053). Console, Ordon village centre: execute 2.2 ms per tick faster with the four on |
| `--vita-fast-gen-mode`, `--vita-fast-pipeline-info` `=off\|on\|verify` | `on` with the profile since 10 Nov | GX worker: a material's gen-mode, colour-channel and texgen writes that leave the decoded state as it was no longer mark the pipeline changed (aurora 0023); the pipeline a lookup has just returned is not compared with the same config again. Console: the pipeline step fell from 10.7 to 5.1 ms per frame in Ordon village centre, from 13.6 to 3.7 in Hyrule Field |
| `--vita-fast-unseen-map=off\|on` | `on` with the profile since 10 Nov | the minimap's polygons are not sent while the backend has no off-screen target to draw them in (dusklight 0052); the picture is the same today. Since 14 Nov the map screens' own pass is sent and drawn on the CPU (dusklight 0073). Hyrule Field sent 351 such draws per frame |
| `--vita-gxm-uniform-direct=off\|on\|verify` | `on` with the profile since 10 Nov | each draw's default uniforms written straight into the reserved buffers, only the vectors it sets; every program's layout is checked against `sceGxmSetUniformDataF` when it is created. Console: about 1 ms per frame off the GX worker |
| `--vita-skip-hidden-draw`, `--vita-skip-offscreen-anim`, `--vita-rest-creatures` `=off\|on` | `off`; the settings page's Speed-ups | picture-only: no actor draw in a game tick that another tick follows before the frame is shown (grass still draws, dusklight 0045); no skeleton update for a few ambient animals while they are off screen (0046); far, unseen fish (no rod out), pumpkins and chickens take the game's own no-execute path until Link comes near (0047) |
| `--vita-crowd=full\|half\|low` | `full`; the settings page's Speed-ups | how many passers Castle Town's crowd keeps walking (dusklight 0048). Resting creatures and a smaller crowd draw fewer random numbers, so random idle choices can differ |
| `--vita-state-digest=off\|on` | `off` | test only: one hash of the game state per tick (every actor's place, angles, speeds, health, status and condition flags; the save data; both random generators), to check that a run with a speed-up on plays the same game as a run with it off (dusklight 0044) |

Added from 10 to 13 Nov (the first seven rows are the defaults that came with 0.9.0):

| Switch | Default | Effect |
| --- | --- | --- |
| `--vita-fast-draw-runs=off\|on\|verify` | `on` with the profile since 13 Nov | a quad or triangle-list draw command followed by more of the same kind, with nothing changed between them, goes to the backend as one draw (the game draws snow, rain, the twilight squares and text one quad per command); `verify` joins nothing and checks each candidate. Console: the Palace of Twilight 7.0 to 13.3 FPS, Snowpeak Ruins room 9 8.7 to 18.9, no mismatch |
| `--vita-fast-shadow-lists=off\|on\|verify` | `on` with the profile since 13 Nov | a simple (blob) shadow's volume is drawn from a display list in which its eight triangle strips are one indexed draw of the same 16 triangles in the same order. Console: Ordon village's widest view 8.7 to 9.9 FPS, no mismatch |
| `--vita-fast-reg-repeats=off\|on\|verify` | `on` with the profile since 13 Nov | a TEV colour or konst write equal to the value last decoded for that register, and a light load equal to the words last decoded, skip their handlers; `verify` runs the handler and compares the state. No mismatch in any console run since 12 Nov |
| `--vita-audio-pull=frame\|subframe` | `frame`; `subframe` with the profile since 13 Nov | how much sound one render call makes: a whole 16.7 ms sound frame, or 80 samples at a time (the same subframes in the same order, spread evenly over the device buffers). Console: late sound buffers in the Palace of Twilight 29 % to 0.2 %, in Hyrule Field's enemy areas 47 % to about a quarter |
| `--vita-fast-particle-palette=off\|on` | `on` with the profile since 13 Nov | child particles, which the game draws one per draw, go out up to ten per draw through the ten position-matrix slots; each vertex is still transformed by its own particle's matrix |
| `--vita-fast-present-bg=off\|on` | `on` with the profile since 13 Nov | the room's background actor does not run its draw a second time in the presentation walk when frame interpolation is off |
| `--vita-fast-pad=off\|on\|verify`, `--vita-fast-pad-events=off\|on\|verify` | `on` with the profile since 10 and 13 Nov | `pad`: PADRead keeps the SDL gamepad values asked since SDL last pumped events, so each button or axis is asked once per pump (console: 2,292 reads compared in verify, no difference). `pad-events`, with `pad` on: the kept values are brought up to date by SDL's own button and axis events instead of being dropped at each pump |
| `--vita-fast-wide-cache`, `--vita-fast-decode-blocks`, `--vita-fast-patient-cache` `=off\|on\|verify` | `on` with the profile since 10 and 11 Nov | geometry cache: a packet is looked for in a group of 8 entries instead of 4; a packet of more than 256 vertices is decoded 256 at a time; a packet is stored the second time its key is seen and survives one frame without use (console: Kakariko stores 16 packets per frame instead of 77). `verify` decodes again and compares |
| `--vita-gxm-submit-thread=off\|on\|verify` | `off` | GXM only: the GX worker hands each finished draw to a third thread that makes the GXM calls. Console: about 2 ms less GX worker time and no FPS gained in the villages, slower in Hyrule Field's enemy areas |
| `--vita-gxm-draw-merge=off\|on\|verify` | `off` | GXM only: a quad or triangle-list draw whose state and sampler requests equal the previous one's byte for byte, and whose vertices follow it directly, joins that draw |
| `--vita-gxm-draw-split=on\|off` | `off` | GXM diagnostic: each draw's backend time split into its steps; several clock reads per draw, so only for a run that measures the split |
| `--vita-audio-priority=default\|high` | `default` | the sound thread's priority; `high` puts it 16 steps above the thread that opens the port. Console, 13 Nov: no gain |
| `--vita-place=X,Y,Z,ANGLE` | none | bench lines only: puts Link at that position once the place has loaded |
| `--vita-sync-loads=off\|on` | `off` | test only: the thread that queues a load request waits until the loader thread has finished it, so loading takes the same number of game ticks in every run and two emulator runs can be compared frame by frame |
| `--vita-disc-read-test=MIB` | none | 1-1024: reads that much of the disc image through its container before the game starts and reports the read speed (log only) |

The play profile also renders at 75 % (`--vita-gxm-scale=75`) unless the command line chooses a
scale: drawn full width, Link's house needs about 67 ms of GPU per frame at 100 % and about 43 at 75 %.

Available in every build:

| Switch | Default | Effect |
| --- | --- | --- |
| `--vita-clocks=legacy444\|system` | `legacy444` | 444/222/222/166 MHz (CPU/bus/GPU/crossbar) through scePower, no plugin; `system` keeps the system's clocks, as a control |
| `--vita-exit-after=SECONDS` | off | 10-3600: the run ends itself that long after Link first appears, with the receipts written |
| `--vita-static-geometry=off\|on\|verify` | `off` | J3D display lists and their arrays by reference (aurora 0006, dusklight 0014); certified cache hits skip the byte comparison; `verify` compares every certified hit and counts differences |
| `--vita-gx-thread=off\|on` | `off` | Aurora's GX FIFO, and every vitaGL draw, on a worker thread on core 1 (aurora 0007); the game thread keeps core 0 |
| `--vita-gpu=vgl\|gxm` | `vgl`; `gxm` with the play profile | the renderer: vitaGL, or the direct GXM backend ([ARCHITECTURE.md](ARCHITECTURE.md)); if it fails to start, the run writes `gfx_init_failure` and exits |
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

## Bench of crowded places

The README's table comes from six runs in one console session, about 10 minutes, hands off: six
crowded places started with `--stage`, each idle for a while and then walking a fixed route
(`--vita-input-script`, `tools/input_scripts/`), with `--vita-receipts=full`. The shader stages of
those runs are prepared at start-up from a warm-up bundle made beforehand in the emulator, so the
console measures frames, not compiles.

`python3 tools/vita_bench_report.py runtime.jsonl` prints, per place and phase (idle, walk), FPS,
ticks per second, main-thread, GX worker and GPU time per frame and which of them is the limit;
`--actors` and `--painter` rank actor types and painter parts by time. Two sessions so far: b1 (the
play profile) and b2 (b1 plus the fast actor paths and the texture pool fix); b2 gained 19 % in Castle
Town and 5 % in Ordon village centre, the other places stayed within noise. The next targets are the
actor types at the top of the ranking (fish in Ordon, the crowd in Castle Town, some enemies), all
with exact fast paths that leave the game's behavior unchanged.
