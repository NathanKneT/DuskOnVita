#!/usr/bin/env python3
"""Extract actual SDK bridge bodies for the standalone recording-GL test."""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vgl_backend_source import exported, function  # noqa: E402

p = argparse.ArgumentParser()
p.add_argument('--output', required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'platform/vita/gfx_vgl/gpu_api.cpp').read_text()
functions = [exported(function(source, name)) for name in ('draw', 'texture', 'texture_static_rgba', 'texture_write')]
Path(a.output).write_text('// Generated from production gpu_api.cpp; do not edit.\n' + '\n'.join(functions) + '\n')
