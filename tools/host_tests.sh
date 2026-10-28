#!/usr/bin/env bash
# Host-compiler tests for platform code that does not need the Vita (Linux/WSL).
#
#   tools/host_tests.sh
#
# gcm_nod_test: the nod C API subset on a synthetic GameCube image built in memory.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
work=${TWV_WORK:-$HOME/twilight-vita-work}
"$repo/tools/fetch_deps.sh" "$work/deps" > /dev/null
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
for compiled in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_GX_FAST_DECODE=$compiled -I "$repo/platform/vita" \
    "$repo/tests/gx_fast_control_test.cpp" "$repo/platform/vita/gfx_vgl/gx_fast_control.cpp" \
    -o "$out/gx_fast_control_$compiled"
  for mode in default off on invalid failed late; do
    "$out/gx_fast_control_$compiled" "$mode"
  done
done
python3 "$repo/tools/gx_decode_verify_fixture.py" --output-dir "$out/gx-oracle"
for compiled in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-deprecated-copy -Wno-parentheses \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DTARGET_PC=1 -DTWV_VITA_GX_FAST_DECODE=$compiled \
    -I "$out/gx-oracle" -I "$out/gx-oracle/include" \
    "$repo/tests/gx_decode_verify_test.cpp" -o "$out/gx_decode_verify_$compiled"
  "$out/gx_decode_verify_$compiled"
done
python3 "$repo/tools/pipeline_info_reuse_fixture.py" --output-dir "$out"
for mode in off on ref; do
  compiled=1; reference=0
  if [[ "$mode" == off ]]; then compiled=0; fi
  if [[ "$mode" == ref ]]; then reference=1; fi
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_GX_FAST_DECODE=$compiled -DTWV_GX_DECODE_REF=$reference \
    -I "$repo/external/dusklight/extern/aurora/include" -I "$out" -I "$repo/platform/vita" \
    "$repo/tests/pipeline_info_reuse_test.cpp" -o "$out/pipeline_info_reuse_$mode"
  "$out/pipeline_info_reuse_$mode"
done
python3 "$repo/tools/fragment_texcoord_test_fixture.py" --output-dir "$out/fragment-cg"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -DFMT_HEADER_ONLY -fsanitize=address,undefined \
  -I "$repo/platform/vita" -I "$repo/external/dusklight/extern/aurora/include" \
  -I "$work/deps/fmt/include" -I "$out/fragment-cg" \
  "$repo/tests/fragment_texcoord_test.cpp" -o "$out/fragment_texcoord_test"
"$out/fragment_texcoord_test"
python3 "$repo/tools/fragment_texcoord_test_fixture.py" --output-dir "$out/fragment-cg-v59" \
  --vita-source-commit bb52f1cf0b0f336c2db1e1da0d3283573da5ac45
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -DFMT_HEADER_ONLY -fsanitize=address,undefined \
  -I "$repo/platform/vita" -I "$repo/external/dusklight/extern/aurora/include" \
  -I "$work/deps/fmt/include" -I "$out/fragment-cg-v59" \
  "$repo/tests/fragment_texcoord_test.cpp" -o "$out/fragment_texcoord_v59_negative"
python3 - "$out/fragment_texcoord_v59_negative" <<'PY'
import signal,subprocess,sys
for mode,reason in [('--coordinates-only','sample.divide==referenceDivide'),
                    ('--unused-stage-only','sample.map==unsigned(s.texMapId)&&sample.coord==unsigned(s.texCoordId)')]:
    result=subprocess.run([sys.argv[1],mode],capture_output=True,text=True)
    assert result.returncode==-signal.SIGABRT,(mode,result.returncode,result.stderr)
    assert reason in result.stderr,(mode,result.stderr)
    assert 'AddressSanitizer:' not in result.stderr and 'runtime error:' not in result.stderr,result.stderr
    print('Expected V59 shader assertion:',mode)
PY
for test in render_bundle_control compact_st resident_geometry memory_policy submit_batch texture_upload texture_conversion_cache system_usage small_packet_cache staging_policy write_class_cache clock_policy fast_shadow_collect; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I "$repo/platform/vita" "$repo/tests/${test}_test.cpp" -o "$out/${test}_test"
  "$out/${test}_test"
done
python3 "$repo/tools/clock_policy_test_fixture.py" --output "$out/clock_entry_fixture.inc"
for benchmark in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_BENCHMARK=$benchmark -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/clock_entry_test.cpp" -o "$out/clock_entry_test_$benchmark"
  "$out/clock_entry_test_$benchmark"
done
# Play profile: the same entry in a build with a GPU selector (every profile path present).
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DTWV_VITA_BENCHMARK=0 -DTWV_TEST_PLAY_PROFILE=1 -I "$repo/platform/vita" -I "$out" \
  "$repo/tests/clock_entry_test.cpp" -o "$out/clock_entry_test_profile"
"$out/clock_entry_test_profile"
python3 "$repo/tools/copy_stream_test_fixture.py" --output-dir "$out/copy-stream"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DTARGET_VITA=1 \
  -I "$out/copy-stream" "$repo/tests/copy_stream_test.cpp" -o "$out/copy_stream_test"
"$out/copy_stream_test"
${CXX:-g++} -std=c++20 -O1 -DTARGET_VITA=1 -Werror=frame-larger-than=8192 -I "$out/copy-stream" \
  -c "$repo/tests/copy_stream_frame_check.cpp" -o "$out/copy_stream_frame_vita.o"
if ${CXX:-g++} -std=c++20 -O1 -DTARGET_VITA=0 -Werror=frame-larger-than=8192 -I "$out/copy-stream" \
    -c "$repo/tests/copy_stream_frame_check.cpp" -o "$out/copy_stream_frame_original.o" 2>/dev/null; then
  echo "copy_stream frame check: the original 64 KiB buffer unexpectedly fits 8 KiB" >&2; exit 1
fi
echo "copy_stream frame check: Vita variant under 8 KiB; original 64 KiB frame rejected"
python3 "$repo/tools/cpu_cost_probe_fixture.py" \
  --source "$repo/platform/vita/gfx_vgl/backend.cpp" --output-dir "$out"
for timers in 0 1; do
  for test in cpu_cost_probe cpu_cost_backend; do
    ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
      -DTWV_VITA_DRAW_TIMERS=$timers -I "$repo/platform/vita" -I "$out" \
      "$repo/tests/${test}_test.cpp" -o "$out/${test}_test_$timers"
    if [[ "$test" == cpu_cost_probe ]]; then
      "$out/${test}_test_$timers" --emit-receipts "$out/cpu_cost_receipts_$timers.jsonl"
      PYTHONPATH="$repo/tools" python3 - "$out/cpu_cost_receipts_$timers.jsonl" "$timers" <<'PY'
import json, sys
from pathlib import Path
from vita_cpu_cost_report import complete_reports, summarize
rows=[json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
reports, partial=complete_reports(rows)
assert len(reports)==1 and not partial
assert summarize(reports)['timers_enabled']==int(sys.argv[2])
print('cpu_cost_report: actual production JSONL agrees with strict consumer')
PY
    else
      "$out/${test}_test_$timers"
    fi
  done
done
python3 "$repo/tools/write_class_test_fixture.py" --source "$repo/platform/vita/gfx_vgl/gpu_api.cpp" \
  --output "$out/write_class_native_fixture.inc"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$out" "$repo/tests/write_class_native_test.cpp" -o "$out/write_class_native_test"
"$out/write_class_native_test"
for compiled in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_RENDER_BUNDLE=$compiled -I "$repo/platform/vita" \
    "$repo/tests/render_bundle_abi_test.cpp" "$repo/platform/vita/gfx_vgl/bundle_options.cpp" \
    -o "$out/render_bundle_abi_$compiled"
  "$out/render_bundle_abi_$compiled"
done
python3 "$repo/tools/render_bundle_sdk_test_fixture.py" --output "$out/render_bundle_sdk_fixture.inc"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$out" "$repo/tests/render_bundle_sdk_test.cpp" -o "$out/render_bundle_sdk_test"
"$out/render_bundle_sdk_test"
python3 "$repo/tools/pinned_vitagl_mip_fixture.py" \
  --source "$work/deps/vitagl/source/utils/gpu_utils.c" \
  --output "$out/pinned_vitagl_mipmap_fixture.inc"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$out" "$repo/tests/pinned_vitagl_mip_test.cpp" -o "$out/pinned_vitagl_mip_test"
"$out/pinned_vitagl_mip_test"
python3 "$repo/tools/vitagl_uniform_test_fixture.py" \
  --archive "$work/deps/_archives/vitagl-7635e3ceb3c52e34ca2e51d2281628d6c756a35b.tar.gz" \
  --output "$out/vitagl_uniform_fixture.inc"
for ffp in 0 1; do
  ffp_flags=(); if ((ffp)); then ffp_flags=(-DHAVE_FFP_SHADER_SUPPORT=1); fi
  ${CC:-gcc} -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    "${ffp_flags[@]}" -I "$out" "$repo/tests/vitagl_uniform_reservation_test.c" -o "$out/vitagl_uniform_test_$ffp"
  "$out/vitagl_uniform_test_$ffp"
done
python3 "$repo/tools/texture_upload_test_fixture.py" "$repo" "$out"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DTWV_TEXTURE_UPLOAD_LIFECYCLE_TEST -I "$out" "$repo/tests/texture_upload_test.cpp" -o "$out/texture_upload_lifecycle"
"$out/texture_upload_lifecycle"
python3 "$repo/tools/clip_pipeline_test_fixture.py" "$repo" "$out"
${CXX:-g++} -std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/clip_bounds_test.cpp" -o "$out/clip_bounds_test"
"$out/clip_bounds_test"
for compiled in 0 1; do
  ${CXX:-g++} -std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_VERTEX_CULL=$compiled -DTWV_VITA_CACHE_VALIDATE=1 -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/clip_pipeline_test.cpp" -o "$out/clip_pipeline_test_$compiled"
  "$out/clip_pipeline_test_$compiled"
done
python3 "$repo/tools/shadow_image_test_fixture.py" "$repo" "$out"
for vita in 0 1; do
  for compiled in 0 1; do
    ${CXX:-g++} -std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
      -DTARGET_PC=1 -DTARGET_VITA=$vita -DTWV_TEST_SHADOW_COMPILED=$compiled \
      -I "$repo/platform/vita" -I "$out" "$repo/tests/shadow_image_test.cpp" -o "$out/shadow_image_${vita}_$compiled"
    "$out/shadow_image_${vita}_$compiled"
  done
done
python3 - "$repo/platform/vita/gfx_vgl/vertex.cpp" "$out/vertex_decoder_selector.inc" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text();start=source.index('extern "C" int twv_vita_stream_decode_select(')
end=source.index('\n}',start)+2
selectors=source[start:end]
start=source.index('extern "C" int twv_vita_neon_compare_select(');end=source.index('\n}',start)+2
selectors+='\n'+source[start:end]
start=source.index('extern "C" int twv_vita_vertex_cull_select(');end=source.index('\n}',start)+2
selectors+='\n'+source[start:end]
for name in ('twv_vita_sparse_spans_select','twv_vita_small_packets_select','twv_vita_uncached_staging_select'):
    start=source.index('extern "C" int '+name+'(');end=source.index('\n}',start)+2
    selectors+='\n'+source[start:end]
Path(sys.argv[2]).write_text(selectors)
PY
for compiled in 0 1; do
  ${CXX:-g++} -std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_TEST_STREAM_COMPILED=$compiled -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/vertex_decoder_control_test.cpp" -o "$out/vertex_decoder_control_$compiled"
  "$out/vertex_decoder_control_$compiled"
done
python3 "$repo/tools/xf_fifo_test_fixture.py" "$repo" "$out"
${CXX:-g++} -std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" -I "$out" "$repo/tests/xf_fifo_test.cpp" -o "$out/xf_fifo_test"
"$out/xf_fifo_test"
mkdir -p "$out/fake-game"
for header in d/actor/d_a_alink.h d/d_com_inf_game.h d/d_item.h dusk/main.h \
              f_op/f_op_actor_mng.h f_op/f_op_overlap_mng.h f_pc/f_pc_manager.h m_Do/m_Do_graphic.h; do
  mkdir -p "$out/fake-game/$(dirname "$header")"; touch "$out/fake-game/$header"
done
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -include "$repo/tests/fixtures/benchmark_game_stub.hpp" \
  -I "$out/fake-game" -I "$repo/platform/vita" \
  "$repo/tests/benchmark_fixture_test.cpp" "$repo/platform/vita/benchmark/fixture.cpp" \
  -o "$out/benchmark_fixture_test"
for mode in off wrong-stage 30hz 60hz variable catchup; do "$out/benchmark_fixture_test" "$mode"; done
python3 - "$repo" "$out/warmup-golden.bin" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,str(Path(sys.argv[1])/'tools'))
from vita_shader_warmup import encode_bundle
Path(sys.argv[2]).write_bytes(encode_bundle([(1,b'vertex_source'),(2,b'fragment_source')]))
PY
for warmup in 0 1; do
  for capture in 0 1; do
    ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
      -DTWV_VITA_SHADER_WARMUP=$warmup -DTWV_VITA_SHADER_SOURCE_CAPTURE=$capture \
      -I "$repo/platform/vita" "$repo/tests/shader_preparation_test.cpp" \
      "$repo/platform/vita/gfx_vgl/shader_preparation.cpp" -o "$out/preparation_$warmup$capture"
    "$out/preparation_$warmup$capture" "$out/warmup-golden.bin" "$out/preparation-data-$warmup$capture"
  done
done
# The bundle packaged in the VPK (TWV_VITA_APP_WARMUP_BUNDLE): app0: only when the data folder has none.
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DTWV_VITA_SHADER_WARMUP=1 -DTWV_VITA_SHADER_SOURCE_CAPTURE=0 -DTWV_VITA_APP_WARMUP_BUNDLE=1 \
  -I "$repo/platform/vita" "$repo/tests/shader_preparation_test.cpp" \
  "$repo/platform/vita/gfx_vgl/shader_preparation.cpp" -o "$out/preparation_app"
"$out/preparation_app" "$out/warmup-golden.bin" "$out/preparation-data-app"
# Start-up screens: disc image checks, the message per problem, font, wrap and progress bar.
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/startup_check_test.cpp" -o "$out/startup_check_test"
"$out/startup_check_test"
python3 "$repo/tools/counter_batch_test_fixture.py" --output-dir "$out"
for batched in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_BATCH_COUNTERS=$batched -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/counter_batch_test.cpp" -o "$out/counter_batch_test_$batched"
  "$out/counter_batch_test_$batched"
done
python3 "$repo/tools/vertex_setup_test_fixture.py" --output-dir "$out/vertex-setup"
for lazy in 0 1; do
  for planned in 0 1; do
    for stream in 0 1; do
      ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-deprecated-copy \
        -DTARGET_PC=1 -DTWV_VITA_LAZY_VERTEX_PLAN=$lazy -DTWV_VITA_PLANNED_FETCH=$planned \
        -DTWV_VITA_STREAM_DECODE=$stream -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I "$repo/platform/vita" -I "$repo/external/dusklight/extern/aurora/include" -I "$out/vertex-setup" \
        "$repo/tests/vertex_setup_test.cpp" -o "$out/vertex_setup_${lazy}_${planned}_${stream}"
      "$out/vertex_setup_${lazy}_${planned}_${stream}"
    done
  done
done
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$work/deps/nodheaders/nod-ffi/include" \
  "$repo/tests/gcm_nod_test.cpp" "$repo/platform/vita/disc/gcm_nod.cpp" -o "$out/gcm_nod_test"
"$out/gcm_nod_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$repo/platform/vita" \
  "$repo/tests/scratch_alloc_test.cpp" -o "$out/scratch_alloc_test"
"$out/scratch_alloc_test"
# G0: the pacing window is shared (gpu_common.cpp); its vitaGL counters are the backend's
# pacing_report (gpu_api.cpp). The harness joins both through a one-entry backend table.
python3 - "$repo/platform/vita/gfx_vgl/gpu_common.cpp" "$repo/platform/vita/gfx_vgl/gpu_api.cpp" \
  "$out/profile_receipts_function.inc" <<'PY'
from pathlib import Path
import sys
common=Path(sys.argv[1]).read_text();start=common.index('void profile_receipts(const TwvGpuBackend& backend,uint64_t now) {')
end=common.index('\nvoid memory_receipts(',start)
vgl=Path(sys.argv[2]).read_text();vstart=vgl.index('void vgl_pacing_report() {')
vend=vgl.index('\nvoid capture_frame(',vstart)
Path(sys.argv[3]).write_text(vgl[vstart:vend]+'\n'+common[start:end])
PY
for timers in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_DRAW_TIMERS=$timers -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/frame_pacing_test.cpp" -o "$out/frame_pacing_test_$timers"
  "$out/frame_pacing_test_$timers"
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_DRAW_TIMERS=$timers -I "$repo/platform/vita" \
    "$repo/tests/draw_profile_test.cpp" -o "$out/draw_profile_test_$timers"
  "$out/draw_profile_test_$timers"
done
# Exercise the real patched dependency function, not a reimplementation of its
# cursor arithmetic. The pinned default build uses the non-speedhack ring.
python3 - "$work/deps/vitagl/source/vgl.c" "$out/vitagl_scratch_function.inc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
start = source.index('uint8_t *vgl_reserve_data_pool(uint32_t size) {')
end = source.index('\n}\n', start) + 3
Path(sys.argv[2]).write_text(source[start:end])
PY
${CC:-gcc} -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$out" "$repo/tests/vitagl_scratch_test.c" -o "$out/vitagl_scratch_test"
"$out/vitagl_scratch_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$repo/platform/vita" \
  "$repo/tests/gpu_vertex_contract_test.cpp" -o "$out/gpu_vertex_contract_test"
"$out/gpu_vertex_contract_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$repo/platform/vita" \
  "$repo/tests/vertex_stream_test.cpp" -o "$out/vertex_stream_test"
"$out/vertex_stream_test"
for probed in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_VERTEX_WORK_PROBE=$probed -I "$repo/platform/vita" \
    "$repo/tests/vertex_work_probe_test.cpp" -o "$out/vertex_work_probe_test_$probed"
  "$out/vertex_work_probe_test_$probed"
done
# Compile the actual patched process_to function in both execution modes. No SDK
# or game build is needed; Thread must retain its original synchronization.
mkdir -p "$out/aurora/lib/gx"
cp "$repo/external/dusklight/extern/aurora/lib/gx/fifo.cpp" "$out/aurora/lib/gx/fifo.cpp"
cp "$repo/external/dusklight/extern/aurora/lib/gx/fifo.hpp" "$out/aurora/lib/gx/fifo.hpp"
patch -s -p1 -d "$out/aurora" < "$repo/patches/aurora/0001-vita-inline-gpu-fifo.patch"
# 0007 (T2) turns the mode into a start-up choice; the same function must behave as before in both.
patch -s -p1 -d "$out/aurora" < "$repo/patches/aurora/0007-vita-gx-worker-thread.patch"
python3 - "$out/aurora/lib/gx/fifo.cpp" "$out/fifo_process_function.inc" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
start=source.index('void process_to(uint64_t target, std::memory_order order) noexcept {')
end=source.index('\n}\n',start)+3
Path(sys.argv[2]).write_text(source[start:end])
PY
for thread in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -DTWV_TEST_THREAD=$thread -I "$out" "$repo/tests/fifo_inline_test.cpp" -o "$out/fifo_test_$thread"
  "$out/fifo_test_$thread"
done
# --vita-frame-overlap (0009's FIFO and frontend files on top of 0001/0007; its command processor
# hooks need the earlier processor patches, which the build applies): the same process_to, and the
# new FIFO functions, extracted from the patched file, with the state machine.
mkdir -p "$out/aurora/lib/dolphin/gx"
cp "$repo/external/dusklight/extern/aurora/lib/dolphin/gx/GXManage.cpp" "$out/aurora/lib/dolphin/gx/"
cp "$repo/external/dusklight/extern/aurora/lib/dolphin/gx/GXAurora.cpp" "$out/aurora/lib/dolphin/gx/"
python3 - "$repo/patches/aurora/0009-vita-frame-overlap.patch" "$out/frame_overlap_fifo.patch" <<'PY'
import re, sys
from pathlib import Path
parts = re.split(r'(?m)^(?=--- a/)', Path(sys.argv[1]).read_text())
keep = [p for p in parts if p.startswith('--- a/') and not p.startswith('--- a/lib/gx/command_processor.cpp')]
assert len(keep) == 4, [p.splitlines()[0] for p in keep]
Path(sys.argv[2]).write_text(''.join(keep))
PY
patch -s -p1 -d "$out/aurora" < "$out/frame_overlap_fifo.patch"
python3 - "$out/aurora/lib/gx/fifo.cpp" "$out/fifo_process_function.inc" "$out/frame_overlap_fifo_functions.inc" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
start=source.index('void process_to(uint64_t target, std::memory_order order) noexcept {')
end=source.index('\n}\n',start)+3
assert Path(sys.argv[2]).read_text()==source[start:end], 'patch 0009 changed process_to'
start=source.index('uint64_t publish_frame() noexcept {')
end=source.index('uint64_t wait_us() noexcept { return sWaitUs; }\n',start)
end=source.index('\n',end)+1
Path(sys.argv[3]).write_text(source[start:end])
PY
for thread in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_TEST_THREAD=$thread -I "$out" -I "$repo/platform/vita" "$repo/tests/frame_overlap_test.cpp" \
    -o "$out/frame_overlap_test_$thread"
  "$out/frame_overlap_test_$thread"
done
# Exercise the production uniform upload/release functions with a GL recorder.
# Both enabled and rollback builds must produce the same live object contents.
python3 - "$repo/platform/vita/gfx_vgl/gpu_api.cpp" "$out" "$repo/tools" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[3])
from vgl_backend_source import exported,function
source=Path(sys.argv[1]).read_text();out=Path(sys.argv[2])
start=source.index('using ProgramKey = ');end=source.index('\nstd::vector<ProgramInfo>',start)
(out/'uniform_program_info.inc').write_text(source[start:end])
start=source.index('void uniform4(');end=source.index('\nGLuint compile_native(',start)
body=source[start:end]
(out/'uniform_submission_functions.inc').write_text(body+'\n'+exported(function(source,'program_delete'))+'\n')
PY
for cached in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_UNIFORM_CACHE=$cached -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/uniform_submission_test.cpp" -o "$out/uniform_submission_test_$cached"
  "$out/uniform_submission_test_$cached"
done
# Compile the real Cg shader-stage ownership and linked-program lifecycle in
# cache and rollback modes. Native-style stage refs keep shared objects alive.
python3 - "$repo/platform/vita/gfx_vgl/gpu_api.cpp" "$out/shader_program_functions.inc" "$repo/tools" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[3])
from vgl_backend_source import exported,function
source=Path(sys.argv[1]).read_text()
start=source.index('GLuint compile_native(');end=source.index('\n}\n\nnamespace {\nint vgl_init()',start)
body=source[start:end]
Path(sys.argv[2]).write_text(body+'\n'+exported(function(source,'program'))+'\n'+
                             exported(function(source,'program_delete'))+'\n')
PY
for cached in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_SHADER_STAGE_CACHE=$cached -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/shader_program_test.cpp" -o "$out/shader_program_test_$cached"
  "$out/shader_program_test_$cached"
done
# Compile the real decoder fetch (and its endian readers), with the previous
# arithmetic and the exact exponent construction, against exhaustive formats.
python3 - "$repo/platform/vita/gfx_vgl/vertex.cpp" "$out/vertex_fetch_function.inc" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
start=source.index('inline uint16_t rd16(')
end=source.index('\ninline V4 fetch_color(',start)
Path(sys.argv[2]).write_text(source[start:end])
PY
for fast in 0 1; do
 for dispatch in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_FAST_FIXED_SCALE=$fast -DTWV_VITA_ATTR_DISPATCH=$dispatch \
    -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/vertex_fetch_test.cpp" -o "$out/vertex_fetch_test_${fast}_${dispatch}"
  "$out/vertex_fetch_test_${fast}_${dispatch}"
 done
done
python3 - "$repo/platform/vita/gfx_vgl/vertex.cpp" "$repo/external/dusklight/extern/aurora/lib/gx/gx.hpp" "$out" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text();gx=Path(sys.argv[2]).read_text();out=Path(sys.argv[3])
data=[]
for name in ('AttrConfig','ColorChannelConfig','TcgConfig'):
    start=gx.index('struct '+name+' {');end=gx.index('\n};',start)+3
    data.append(gx[start:end])
(out/'vertex_stage_types.inc').write_text('\n'.join(data))
start=source.index('struct V3 {');end=source.index('inline float dot3(',start)
parts=[source[start:end]]
start=source.index('inline uint16_t rd16(');end=source.index('struct Decoded {',start)
parts.append(source[start:end])
start=end;end=source.index('class VertexStage {',start);parts.append(source[start:end])
start=end;end=source.index('  // GX lighting_func',start);parts.append(source[start:end])
start=source.index('  unsigned source_spans(');end=source.index('  uint32_t indexed_array_mask(',start)
parts.append(source[start:end])
start=source.index('  const aurora::gx::ShaderConfig& cfg;',end)
end=source.index('};\n} // namespace',start)+2;parts.append(source[start:end])
(out/'vertex_stage_decode.inc').write_text('\n'.join(parts))
(out/'vertex_scalar_color_reference.inc').write_text(source[source.index('inline uint16_t rd16('):source.index('struct Decoded {')])
PY
python3 "$repo/tools/small_packet_test_fixture.py" --output-dir "$out"
for lazy in 0 1; do
 for validate in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTARGET_PC=1 -DTWV_VITA_LAZY_VERTEX_PLAN=$lazy -DTWV_VITA_CACHE_VALIDATE=$validate -I "$repo/platform/vita" -I "$out" \
    -I "$repo/external/dusklight/extern/aurora/include" \
    "$repo/tests/small_packet_integration_test.cpp" -o "$out/small_packet_integration_test_${lazy}_$validate"
  "$out/small_packet_integration_test_${lazy}_$validate"
 done
done
for planned in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTARGET_PC=1 -DTWV_VITA_PLANNED_FETCH=$planned -DTWV_VITA_FAST_FIXED_SCALE=1 -DTWV_VITA_ATTR_DISPATCH=1 \
    -I "$repo/platform/vita" -I "$out" -I "$repo/external/dusklight/extern/aurora/include" \
    "$repo/tests/vertex_stage_decode_test.cpp" -o "$out/vertex_stage_decode_test_$planned"
  "$out/vertex_stage_decode_test_$planned"
done
${CXX:-g++} -std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DTARGET_PC=1 -DTWV_VITA_PLANNED_FETCH=0 -DTWV_VITA_FAST_FIXED_SCALE=1 -DTWV_VITA_ATTR_DISPATCH=1 \
  -I "$repo/platform/vita" -I "$out" -I "$repo/external/dusklight/extern/aurora/include" \
  "$repo/tests/source_span_coverage_test.cpp" -o "$out/source_span_coverage_test"
"$out/source_span_coverage_test"
${CXX:-g++} -std=c++20 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DTARGET_PC=1 -DTWV_VITA_FAST_FIXED_SCALE=1 -DTWV_VITA_ATTR_DISPATCH=1 \
  -I "$repo/platform/vita" -I "$out" -I "$repo/external/dusklight/extern/aurora/include" \
  "$repo/tests/attribute_stream_bounds_test.cpp" -o "$out/attribute_stream_bounds_test"
"$out/attribute_stream_bounds_test"
for test in compact_stream_write small_packet_policy; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTARGET_PC=1 -I "$repo/platform/vita" -I "$repo/external/dusklight/extern/aurora/include" \
    "$repo/tests/${test}_test.cpp" -o "$out/${test}_test"
  "$out/${test}_test"
done
python3 - "$repo/platform/vita/gfx_vgl/vertex.cpp" "$out/vertex_color_functions.inc" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
readers=source[source.index('inline uint16_t rd16('):source.index('// Components of a non-colour attribute')]
colors=source[source.index('inline V4 fetch_color('):source.index('\nstruct Decoded {')]
Path(sys.argv[2]).write_text(readers+'\n'+colors)
PY
for lookup in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_COLOR_LOOKUP=$lookup -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/vertex_color_test.cpp" -o "$out/vertex_color_test_$lookup"
  "$out/vertex_color_test_$lookup"
done
for words in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_WORD_COMPARE=$words -I "$repo/platform/vita" \
    "$repo/tests/exact_bytes_test.cpp" -o "$out/exact_bytes_test_$words"
  "$out/exact_bytes_test_$words"
  for ways in 2 4; do
    ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
      -DTWV_VITA_WORD_COMPARE=$words -DTWV_VITA_GEOMETRY_CACHE_WAYS=$ways \
      -I "$repo/platform/vita" "$repo/tests/geometry_cache_test.cpp" -o "$out/geometry_cache_test_${ways}_$words"
    "$out/geometry_cache_test_${ways}_$words"
  done
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_WORD_COMPARE=$words -I "$repo/platform/vita" \
    "$repo/tests/static_geometry_test.cpp" -o "$out/static_geometry_test_$words"
  "$out/static_geometry_test_$words"
done
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/light_plan_test.cpp" -o "$out/light_plan_test"
"$out/light_plan_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/fight_timeline_test.cpp" -o "$out/fight_timeline_test"
"$out/fight_timeline_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/cache_budget_test.cpp" -o "$out/cache_budget_test"
"$out/cache_budget_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/vertex_staging_test.cpp" -o "$out/vertex_staging_test"
"$out/vertex_staging_test"
# Compile actual SDK-facing sampler/upload/delete functions against a recording
# GL context, in both optimized and rollback modes. Test object state and ID
# lifetime, not a second implementation of the cache's lookup algorithm.
python3 - "$repo/platform/vita/gfx_vgl/gpu_api.cpp" "$out/texture_sampler_functions.inc" "$repo/tools" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[3])
from vgl_backend_source import exported,function
source=Path(sys.argv[1]).read_text()
sections=[source[source.index('GLenum wrap(int n) {'):source.index('\nvoid uniform4(',source.index('GLenum wrap(int n) {'))]]
for name in ('texture_write','texture_delete','texture_sampler'):
    sections.append(exported(function(source,name))+'\n')
Path(sys.argv[2]).write_text('\n'.join(sections))
PY
for cached in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_SAMPLER_CACHE=$cached -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/texture_sampler_test.cpp" -o "$out/texture_sampler_test_$cached"
  "$out/texture_sampler_test_$cached"
done
# Per-unit sampler control and the actual draw body, with recording GL only.
python3 "$repo/tools/sampler_unit_test_fixture.py" --output-dir "$out/sampler-unit"
for cached in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_SAMPLER_CACHE=$cached -I "$repo/platform/vita" -I "$out/sampler-unit" \
    "$repo/tests/sampler_unit_test.cpp" -o "$out/sampler_unit_test_$cached"
  "$out/sampler_unit_test_$cached"
done
python3 "$repo/tools/sampler_unit_backend_fixture.py" --repo-root "$repo" --output-dir "$out/sampler-backend"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -DTARGET_PC=1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$out/sampler-backend" -I "$repo/external/dusklight/extern/aurora/include" \
  "$repo/tests/sampler_unit_backend_test.cpp" -o "$out/sampler_unit_backend_test"
"$out/sampler_unit_backend_test"

# Real pinned driver functions and all six native texture-binding loops.
# The setter copies descriptors in this host seam; native GXM/pixels remain unproved.
for version in candidate baseline; do
  sampler_fixture_flags=()
  if [[ "$version" == baseline ]]; then sampler_fixture_flags=(--baseline); fi
  python3 "$repo/tools/pinned_sampler_driver_fixture.py" \
    --archive "$work/deps/_archives/vitagl-7635e3ceb3c52e34ca2e51d2281628d6c756a35b.tar.gz" \
    --reservation-patch "$repo/patches/deps/vitagl/0001-sdk-managed-uniform-reservations.patch" \
    --output-dir "$out/sampler-driver-$version" "${sampler_fixture_flags[@]}"
done
for skip in 0 1; do
  sampler_error_flags=()
  if ((skip)); then sampler_error_flags=(-DSKIP_ERROR_HANDLING=1); fi
  for version in candidate baseline; do
    ${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-parentheses \
      -fsanitize=address,undefined -fno-omit-frame-pointer "${sampler_error_flags[@]}" \
      -I "$out/sampler-driver-$version" "$repo/tests/pinned_sampler_driver_test.cpp" \
      -o "$out/sampler_driver_${version}_$skip"
  done
  "$out/sampler_driver_candidate_$skip"
  python3 - "$out/sampler_driver_baseline_$skip" "$skip" <<'PY'
import os,signal,subprocess,sys
for mode in ('--non-mip-only','--oom-only'):
    env=os.environ.copy()
    env['UBSAN_OPTIONS']='halt_on_error=1'
    result=subprocess.run([sys.argv[1],mode],capture_output=True,text=True,env=env)
    if mode=='--non-mip-only':
        assert result.returncode==-signal.SIGABRT,(mode,result.returncode,result.stderr)
        assert 'count_bits(capture.descriptor)==row.count' in result.stderr,result.stderr
        assert 'AddressSanitizer:' not in result.stderr and 'runtime error:' not in result.stderr,result.stderr
    else:
        assert result.returncode==1,(mode,result.returncode,result.stderr)
        assert "member access within null pointer of type 'struct sampler'" in result.stderr,result.stderr
    print('Expected original sampler rejection:',sys.argv[2],mode)
PY
done
python3 - "$repo/platform/vita/gfx_vgl/gpu_api.cpp" "$out/raster_submission_functions.inc" "$repo/tools" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,sys.argv[3])
from vgl_backend_source import exported,function
source=Path(sys.argv[1]).read_text()
start=source.index('GLenum factor(');end=source.index('\nGLenum wrap(',start)
parts=[source[start:end]]
parts.append(exported(function(source,'begin'))+'\n')
parts.append(exported(function(source,'clear'))+'\n')
Path(sys.argv[2]).write_text('\n'.join(parts))
PY
for cached in 0 1; do
  ${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DTWV_VITA_RASTER_CACHE=$cached -I "$repo/platform/vita" -I "$out" \
    "$repo/tests/raster_submission_test.cpp" -o "$out/raster_submission_test_$cached"
  "$out/raster_submission_test_$cached"
done
# G0: draw digest at the backend interface, GPU start-up options and capture schedule.
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" -I "$work/deps/xxhash" \
  "$repo/tests/draw_digest_test.cpp" -o "$out/draw_digest_test"
"$out/draw_digest_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/gpu_options_test.cpp" -o "$out/gpu_options_test"
"$out/gpu_options_test"
# G1: GXM state mappings, memory bookkeeping and the GXP cache format.
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" -I "$work/deps/xxhash" \
  "$repo/tests/gxm_state_test.cpp" -o "$out/gxm_state_test"
"$out/gxm_state_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/decode_probe_test.cpp" -o "$out/decode_probe_test"
"$out/decode_probe_test"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/pipeline_fast_test.cpp" -o "$out/pipeline_fast_test"
"$out/pipeline_fast_test"
# GPU timing diagnostic: per-frame metrics from submit/notification/present/flip times.
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/gxm_timing_test.cpp" -o "$out/gxm_timing_test"
"$out/gxm_timing_test"
# Vita fixed game ticks (patches/dusklight/0015) on the pinned game clock and Aurora's clock.
mkdir -p "$out/clock/src/dusk" "$out/clock/src/m_Do"
cp "$repo/external/dusklight/src/dusk/game_clock.cpp" "$repo/external/dusklight/src/dusk/game_clock.h" "$out/clock/src/dusk/"
cp "$repo/external/dusklight/src/m_Do/m_Do_main.cpp" "$out/clock/src/m_Do/"
patch -s -p1 -d "$out/clock" < "$repo/patches/dusklight/0015-vita-fixed-game-ticks.patch"
# 0019's game clock part (its m_Do_main.cpp part needs the earlier main-loop patches).
python3 - "$repo/patches/dusklight/0019-vita-frame-overlap-settle.patch" "$out/clock_0019.patch" <<'PY'
import re, sys
from pathlib import Path
parts = re.split(r'(?m)^(?=--- a/)', Path(sys.argv[1]).read_text())
keep = [p for p in parts if p.startswith('--- a/src/dusk/game_clock.cpp')]
assert len(keep) == 1
Path(sys.argv[2]).write_text(keep[0])
PY
patch -s -p1 -d "$out/clock" < "$out/clock_0019.patch"
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DTARGET_VITA=1 \
  -I "$out/clock/src" -I "$repo/external/dusklight/extern/aurora/include" \
  -I "$repo/external/dusklight/extern/aurora/lib" -I "$repo/tests/fake_sdl" \
  "$repo/tests/game_clock_test.cpp" "$repo/external/dusklight/extern/aurora/lib/time.cpp" -o "$out/game_clock_test"
"$out/game_clock_test"
# Test-only scripted pad input (--vita-input-script).
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/input_script_test.cpp" -o "$out/input_script_test"
"$out/input_script_test"
# Front touch for the menu pointer: finger tracking and the drawn area it maps to.
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/touch_test.cpp" -o "$out/touch_test"
"$out/touch_test" "$repo/platform/vita/gfx_vgl/backend.cpp"
# game.buttonPrompts=playstation: glyphs drawn in code as GX textures (patches/dusklight/0016).
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/platform/vita/ui/ps_glyphs.cpp" "$repo/tests/ps_glyphs_test.cpp" \
  -o "$out/ps_glyphs_test"
"$out/ps_glyphs_test"
# Main-thread actor profile: type slots, distance bins, top-N choice, receipt text, sampling.
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$repo/platform/vita" "$repo/tests/actor_profile_stats_test.cpp" -o "$out/actor_profile_stats_test"
"$out/actor_profile_stats_test"
# Save safety (patches/aurora/0011, docs/ARCHITECTURE.md): backups, the boot check and the
# Save import on real folders, and the real patched card with and without the port's hooks.
TWV_WORK="$work" bash "$repo/tools/save_safety_tests.sh" "$out/save-safety"
