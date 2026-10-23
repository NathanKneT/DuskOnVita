#!/usr/bin/env python3
"""Real Cg generators and pinned upstream sampling/projection decision seams.

This is a source-semantic host test; it does not compile native Cg or draw GPU
pixels. Compose the existing pinned real ShaderInfo/type fixture rather than
substituting a second implementation of dependency analysis.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
from pipeline_info_reuse_fixture import AURORA, ROOT, function, pristine, struct


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--vita-source-commit',choices=['bb52f1cf0b0f336c2db1e1da0d3283573da5ac45'],
                        help='V59 negative control only; the uncorrected generator must fail the new tests')
    args=parser.parse_args()
    out=args.output_dir
    out.mkdir(parents=True,exist_ok=True)
    subprocess.run([sys.executable,str(ROOT/'tools/pipeline_info_reuse_fixture.py'),
                    '--output-dir',str(out)],check=True)
    original=pristine('lib/gx/shader.cpp')
    # Source pin binds the extracted original branch and predicate. Re-audit on
    # upstream changes instead of silently switching the expected behavior.
    expected='baa10f92ae26c716623ba3a51a763661ad5146735c1163899bbd196f59899652'
    actual=hashlib.sha256(original.encode()).hexdigest()
    if actual!=expected:raise SystemExit('Unexpected Aurora shader.cpp: '+actual)
    start=original.index('    if (tcg.type == GX_TG_MTX3x4) {', original.index('// Apply line/point tex offset'))
    end=original.index('\n  }\n  // Multiple TEV',start)
    reference='namespace reference {\nusing namespace aurora::gx;\n'
    reference+=function(original,'bool uses_texture_sample(')+'\n'
    reference+='std::string coordinate(const TcgConfig& tcg,unsigned i) {\nstd::string vtxXfrAttrs,fragmentFnPre;\n'
    reference+=original[start:end]+'\nreturn fragmentFnPre;\n}\n}\n'
    (out/'fragment_coordinate_reference.inc').write_text(reference)
    internal=(ROOT/'platform/vita/gfx_vgl/internal.hpp').read_text()
    (out/'fragment_pipeline.inc').write_text(struct(internal,'struct Pipeline {'))
    for name in ('shaders.cpp','gpu_vertex_shader.cpp'):
        base=(ROOT/'tests/fixtures/baseline'/args.vita_source_commit[:7] if args.vita_source_commit else ROOT)
        source=(base/'platform/vita/gfx_vgl'/name).read_text()
        source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))+'\n'
        (out/(name+'.inc')).write_text(source)
    print('Fragment fixture: actual Cg generators, real ShaderInfo and hash-pinned original projection/sample rules')


if __name__=='__main__':main()
