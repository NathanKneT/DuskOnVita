#!/usr/bin/env python3
"""Extract actual SDK bridge bodies for the standalone recording-GL test."""
import argparse
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('--output', required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'platform/vita/gfx_vgl/gpu_api.cpp').read_text()
functions = []
for name in ('twv_vgl_draw', 'twv_vgl_texture', 'twv_vgl_texture_static_rgba', 'twv_vgl_texture_write'):
    import re
    match = re.search(r'extern "C" [^\n]*\b' + name + r'\s*\(', source)
    if not match:
        raise SystemExit('Missing production function: ' + name)
    start = match.start()
    body = source.index('{', match.end())
    depth = 1
    end = body + 1
    while depth:
        if source[end] == '{': depth += 1
        elif source[end] == '}': depth -= 1
        end += 1
    functions.append(source[start:end])
Path(a.output).write_text('// Generated from production gpu_api.cpp; do not edit.\n' + '\n'.join(functions) + '\n')
