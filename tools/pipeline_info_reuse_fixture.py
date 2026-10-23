#!/usr/bin/env python3
"""Extract the real patched draw frontend, original builder/types and Vita callback.

Only the external pipeline/storage/texture/uniform submission dependencies are
recorded by the host test. No substitute ShaderInfo builder is supplied.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
AURORA = ROOT / 'external/dusklight/extern/aurora'
SOURCE = 'lib/gx/command_processor.cpp'
BASELINE_SHA = 'ff3eeb1c06562c0cbbeb5cbad0daa45aeb4370733dd073ccd34f10f6303ba5ab'


def pristine(name):
    return subprocess.check_output(['git', '-C', str(AURORA), 'show', 'HEAD:' + name], text=True)


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


def struct(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n};', start) + 3]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output-dir', type=Path, required=True)
    args = p.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    original = pristine(SOURCE)
    if hashlib.sha256(original.encode()).hexdigest() != BASELINE_SHA:
        raise SystemExit('Unexpected pinned Aurora command_processor.cpp; re-audit fixture')
    patch = ROOT / 'patches/aurora/0004-vita-pipeline-info-reuse.patch'
    with tempfile.TemporaryDirectory(prefix='twv-shader-info-') as temp:
        for name in (SOURCE, 'lib/gx/regs.hpp', 'lib/gx/regs.cpp'):
            target = Path(temp) / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((original if name == SOURCE else pristine(name)).encode())
        for candidate in (ROOT / 'patches/aurora/0003-vita-fast-register-decode.patch', patch):
            # Canonical patches may have CRLF in a Windows checkout. Normalize
            # both sides in the temporary fixture, retaining strict hunk and
            # whitespace checks on Linux and Windows alike.
            subprocess.run(['git', 'apply', '--whitespace=error', '-'],
                           input=candidate.read_bytes().replace(b'\r\n', b'\n'), cwd=temp, check=True)
        target = Path(temp) / SOURCE
        changed = target.read_text()
    gx = pristine('lib/gx/gx.hpp')
    pipe = pristine('lib/gx/pipeline.hpp')
    shader = pristine('lib/gx/shader_info.cpp')
    backend = (ROOT / 'platform/vita/gfx_vgl/backend.cpp').read_text()
    constants = gx[gx.index('constexpr u32 MaxTextures'):gx.index('extern wgpu::BindGroup')]
    tev = gx[gx.index('template <typename Arg, Arg Default>'):gx.index('// For uniform generation')]
    light_fog = gx[gx.index('struct Light {'):gx.index('struct AttrArray {')]
    types = (constants + tev + struct(gx, 'struct TcgConfig {') + '\n'
             + struct(gx, 'struct TevSwap {') + '\n' + struct(gx, 'struct AlphaCompare {') + '\n'
             + struct(gx, 'struct AttrConfig {') + '\n' + struct(gx, 'struct ShaderConfig {') + '\n'
             + struct(gx, 'struct ShaderInfo {') + '\n' + struct(gx, 'struct GXBindGroups {') + '\n'
             + light_fog + pipe[pipe.index('constexpr uint32_t GXPipelineConfigVersion'):pipe.index('\n', pipe.index('constexpr uint32_t GXPipelineConfigVersion'))] + '\n'
             + struct(pipe, 'struct PipelineConfig {') + '\n'
             + struct(pipe, 'struct DrawData {'))
    # Production representation guarantees used by the callback's config equality.
    types += '\nstatic_assert(std::has_unique_object_representations_v<ShaderConfig>);\n'
    types += 'static_assert(std::has_unique_object_representations_v<PipelineConfig>);\n'
    builder = (shader[shader.index('bool is_alpha_bump_channel'):shader.index('\n', shader.index('bool is_alpha_bump_channel'))]
               + '\n' + function(pristine('lib/gx/attr_fmt.cpp'), 'u8 color_channel(')
               + '\n' + function(shader, 'void color_arg_reg_info(')
               + '\n' + function(shader, 'void alpha_arg_reg_info(')
               + '\n' + function(shader, 'ShaderInfo build_shader_info(').replace(
                   'ShaderInfo build_shader_info(', 'ShaderInfo build_shader_info_original(', 1))
    metadata = '// Real production extraction; pinned frontend SHA256 ' + BASELINE_SHA + '\n'
    (args.output_dir / 'pipeline_info_types.inc').write_text(metadata + 'namespace aurora::gx {\n' + types + '\n}\n')
    align_start = backend.index('uint32_t align_uniform(')
    align = backend[align_start:backend.index('\n', align_start)]
    (args.output_dir / 'pipeline_info_builder.inc').write_text(
        metadata + 'namespace aurora::gfx {\n' + align + '\n}\nnamespace aurora::gx {\n' + builder + '\n}\n')
    stats_start = backend.index('struct PipelineInfoReuseStats {')
    stats_end = backend.index('} sPipelineInfoReuse;', stats_start) + len('} sPipelineInfoReuse;')
    stats = backend[stats_start:stats_end]
    callback = (function(backend, 'bool same_shader_info(') + '\n'
                + function(backend, '[[noreturn]] void pipeline_info_failure(') + '\n'
                + function(backend, 'extern "C" void twv_vita_pipeline_info_built(') + '\n'
                + function(backend, 'extern "C" int twv_vita_pipeline_info_copy('))
    (args.output_dir / 'pipeline_info_callback.inc').write_text(metadata + stats + '\n' + callback + '\n')
    cache = struct(original, 'struct FogRangeLutKey {') + '\n' + struct(original, 'struct DrawCache {')
    draw = function(changed, 'static void push_gx_draw(')
    reference = function(original, 'static void push_gx_draw(').replace('push_gx_draw(', 'push_gx_draw_reference(', 1)
    draw_cache = ('namespace aurora::gx::fifo {\n' + cache + '\nDrawCache sDrawCache;\n'
                  + function(original, 'u8 line_mode_for_prim(') + '\n}\n')
    (args.output_dir / 'pipeline_info_draw_cache.inc').write_text(metadata + draw_cache)
    (args.output_dir / 'pipeline_info_draw.inc').write_text(metadata + 'namespace aurora::gx::fifo {\n' + draw + '\n' + reference + '\n}\n')
    print('Extracted original ShaderInfo builder/types, patched frontend and production callback into', args.output_dir)


if __name__ == '__main__':
    main()
