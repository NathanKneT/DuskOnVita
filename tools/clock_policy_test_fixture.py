#!/usr/bin/env python3
"""Compile the real Vita entry against recording native calls, without an SDK.

Only SDK includes are removed. The entry, argument parser, renderer selection,
clock policy calls and telemetry are tested intact rather than reimplemented.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--output', required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'platform/vita/main_vita.cpp').read_text()
for header in ('psp2/kernel/processmgr.h', 'psp2/kernel/sysmem.h', 'psp2/power.h'):
    include = f'#include <{header}>\n'
    if source.count(include) != 1:
        raise SystemExit(f'Expected one SDK include: {header}')
    source = source.replace(include, '')
Path(args.output).write_text('// Generated from production main_vita.cpp; do not edit.\n' + source)
