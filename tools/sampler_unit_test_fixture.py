#!/usr/bin/env python3
"""Extract actual private bridge bodies for a recording GL dependency seam.

This does not extract or replace the native driver. Its separate pinned-driver
fixture must qualify transactional creation and 2D non-mip descriptor encoding.
Execute only after the parent releases the focused-host execution window.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'platform/vita/gfx_vgl/gpu_api.cpp'
REFERENCE_COMMIT='964d278cd1190f659c23bc68be5f62b32a7da245'
REFERENCE_SHA256='2752633cfabc5af31b7d533418f318a735aa6e596d9287eba2248194bad2d589'


def function(source,signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]


def marked(source,tag):
    start=source.index('// '+tag+'_BEGIN')
    end=source.index('// '+tag+'_END',start)
    return source[source.index('\n',start)+1:end]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--reference-source',type=Path,
                        help='Private-draft baseline override only; content must match the pinned V60 blob')
    args=parser.parse_args()
    args.output_dir.mkdir(parents=True,exist_ok=True)
    source=SOURCE.read_text()
    pieces={
        'sampler_unit_state.inc':marked(source,'C03_SAMPLER_UNIT_STATE'),
        'sampler_unit_helpers.inc':marked(source,'C03_SAMPLER_UNIT_HELPERS'),
        'sampler_unit_draw.inc':function(source,'extern "C" void twv_vgl_draw('),
        'sampler_unit_shutdown.inc':function(source,'extern "C" void twv_vgl_shutdown('),
    }
    names=('extern "C" int twv_vgl_texture_sampler_unit(',
           'extern "C" void twv_vgl_texture_write(',
           'extern "C" void twv_vgl_texture_delete(',
           'extern "C" void twv_vgl_texture_sampler(')
    pieces['sampler_unit_api.inc']='\n'.join(function(source,name) for name in names)
    start=source.index('using ProgramKey = ')
    end=source.index('\n};',source.index('struct ProgramInfo {',start))+3
    pieces['sampler_unit_program.inc']=source[start:end]
    init=function(source,'extern "C" int twv_vgl_init(')
    shutdown=pieces['sampler_unit_shutdown.inc']
    # Full init remains SDK-dependent. Audit actual placement: sampler failure
    # changes ready before the final initialized receipt and before buffer use.
    assert init.index('ready = viewport[2]==960 && viewport[3]==544;')<init.index('initialize_unit_samplers()')
    assert init.index('initialize_unit_samplers()')<init.index('telemetryEvent("gfx_initialized", ready)')
    assert init.index('telemetryEvent("gfx_initialized", ready)')<init.index('if (!ready) return 0;')
    assert init.index('if (!ready) return 0;')<init.index('glGenBuffers(')
    assert shutdown.index('glFinish();')<shutdown.index('release_unit_samplers();')<shutdown.index('glDeleteBuffers(')
    # Keep old production fixture spans byte-identical where no new behavior is
    # intended. Only the draw fixture needs explicit sampler boundary adapters.
    baseline=(args.reference_source.read_text() if args.reference_source else
              (ROOT/'tests/fixtures/baseline'/REFERENCE_COMMIT[:7]/'platform/vita/gfx_vgl/gpu_api.cpp').read_text())
    if hashlib.sha256(baseline.encode()).hexdigest()!=REFERENCE_SHA256:
        raise SystemExit('Unexpected frozen V60 gpu_api.cpp; re-audit sampler conversion oracle')
    for signature in names[1:]:
        assert function(source,signature)==function(baseline,signature)
    wrap='GLenum wrap(int n) {'
    assert source[source.index(wrap):source.index('\n',source.index(wrap))]==baseline[baseline.index(wrap):baseline.index('\n',baseline.index(wrap))]
    for name,body in pieces.items():
        (args.output_dir/name).write_bytes((body+'\n').encode())
    manifest={'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              'reference_commit':REFERENCE_COMMIT,'reference_sha256':REFERENCE_SHA256,
              'actual_extracted_bodies':list(pieces),
              'full_init_sha256':hashlib.sha256(init.encode()).hexdigest(),
              'scope':'real bridge control/conversions/cache/draw/teardown; recording GL only; no native descriptor/pixel/ABI evidence',
              'driver_prerequisites':'transactional guarded glGenSamplers; pinned 2D non-mip count1 override+restore'}
    (args.output_dir/'sampler_unit_fixture_manifest.json').write_bytes((json.dumps(manifest,indent=2)+'\n').encode())


if __name__=='__main__':main()
