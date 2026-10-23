#!/usr/bin/env bash
# Performance-test VPK: every startup-selectable renderer path compiled in
# (selected per run in args.txt / sequence.txt, default OFF), diagnostics that
# cost per draw compiled out, frame/inclusive receipts and scene probe kept.
#
#   tools/vita_perf_build.sh [build-dir]      (default build-vita-perf)
#
# Uses its own work tree (TWV_WORK, default ~/twilight-vita-perf-work), so it never
# shares object files with another checkout's build.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
export TWV_WORK=${TWV_WORK:-$HOME/twilight-vita-perf-work}
build=${1:-build-vita-perf}
exec bash "$repo/tools/vita_build.sh" "$build" \
  -DTWV_VITA_GFX_VGL=ON -DTWV_VITA_RENDER_BUNDLE=ON -DTWV_VITA_GX_FAST_DECODE=ON \
  -DTWV_VITA_LAZY_VERTEX_PLAN=ON -DTWV_VITA_STREAM_DECODE=ON -DTWV_VITA_SMALL_PACKET_CACHE=ON \
  -DTWV_VITA_UNCACHED_STAGING=ON -DTWV_VITA_NEON_COMPARE=ON -DTWV_VITA_SPARSE_SPANS=ON \
  -DTWV_VITA_VERTEX_CULL=ON -DTWV_VITA_SHADOW_IMAGE_SKIP=ON -DTWV_VITA_SHADER_WARMUP=ON \
  -DTWV_VITA_SCENE_PROBE=ON \
  -DTWV_VITA_DRAW_TIMERS=OFF -DTWV_VITA_VERTEX_WORK_PROBE=OFF -DTWV_VITA_CACHE_VALIDATE=OFF \
  -DTWV_VITA_CAPTURE_FRAMES=OFF -DTWV_VITA_SHADER_SOURCE_CAPTURE=OFF -DTWV_VITA_PLANNED_FETCH=OFF \
  -DTWV_VITA_BENCHMARK=OFF
