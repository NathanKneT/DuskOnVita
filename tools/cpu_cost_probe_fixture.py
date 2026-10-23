#!/usr/bin/env python3
"""Extract production backend functions and their diagnostic-free reference.

The reference removes only the named probe statements (and restores the existing
pipeline scope). The test supplies deterministic CPU dependencies and storage;
it validates output/routing and counters, not a shader or GPU result.
"""
import argparse
from pathlib import Path


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


def reference(source: str, replacements: dict[str, str]) -> str:
    for old, new in replacements.items():
        if source.count(old) != 1:
            raise SystemExit(f'Production probe statement changed; re-audit fixture: {old}')
        source = source.replace(old, new, 1)
    if 'cpuCost' in source:
        raise SystemExit('Unrecognized CPU cost probe statement in production function')
    return source


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text()
    descriptor = function(source, 'void populate_pipeline_config(')
    indices = function(source, 'Range push_indices(')
    pipeline = function(source, 'template<> PipelineRef pipeline_ref<gx::PipelineConfig>(')
    descriptor_ref = reference(descriptor, {
        '  twv::vgl::CpuDescriptorScope cpuCost(sCpuCostProbe,sizeof config);\n': '',
    }).replace('populate_pipeline_config(', 'populate_pipeline_config_reference(', 1)
    indices_ref = reference(indices, {
        ' twv::vgl::CpuIndexScope cpuCost(sCpuCostProbe,size);\n': '',
        ' cpuCost.allocated(copy!=nullptr);\n': '',
        ' cpuCost.copied();\n': '',
    }).replace('push_indices(', 'push_indices_reference(', 1)
    pipeline_ref = reference(pipeline, {
        ' twv::vgl::CpuPipelineScope cpuCost(sCpuCostProbe,sizeof config);':
        ' twv::vgl::DrawProfileScope<TWV_PROFILE_PIPELINE> timer;',
        ' if(existing!=UINT32_MAX) {cpuCost.found();return existing;}':
        ' if(existing!=UINT32_MAX) return existing;',
        ' cpuCost.missing();\n': '',
        ' cpuCost.shader_info_done();\n': '',
        ' cpuCost.layout_done();\n': '',
        'cache.push_back(std::move(p));cpuCost.created();return':
        'cache.push_back(std::move(p));return',
    })
    signature = 'template<> PipelineRef pipeline_ref<gx::PipelineConfig>('
    pipeline = pipeline.replace(signature, 'PipelineRef resolve_pipeline(', 1)
    pipeline_ref = pipeline_ref.replace(signature, 'PipelineRef resolve_pipeline_reference(', 1)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / 'cpu_cost_backend_fixture.inc').write_text(
        '// Extracted from platform/vita/gfx_vgl/backend.cpp; do not edit.\n'
        'namespace aurora::gx {\n' + descriptor + '\n' + descriptor_ref + '\n}\n'
        'namespace aurora::gfx {\n' + indices + '\n' + indices_ref + '\n'
        + pipeline + '\n' + pipeline_ref + '\n}\n')


if __name__ == '__main__':
    main()
