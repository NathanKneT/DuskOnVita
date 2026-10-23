#!/usr/bin/env python3
"""Extract Aurora's real write_exact/copy_stream before and after patch 0005.

original.inc comes from the pinned submodule's lib/io.cpp, candidate.inc from
the same file with patches/aurora/0005-vita-copy-stream-stack.patch applied by
patch(1) in a temporary directory. tests/copy_stream_test.cpp compares them.
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--output-dir', required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
aurora = root / 'external/dusklight/extern/aurora'
patch = root / 'patches/aurora/0005-vita-copy-stream-stack.patch'


def function(source, signature):
    if source.count(signature) != 1:
        raise SystemExit(f'Expected one definition: {signature}')
    start = source.index(signature)
    depth = 0
    # Neither function has braces inside string literals or comments.
    for end in range(source.index('{', start), len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if depth == 0:
                return source[start:end + 1] + '\n'
    raise SystemExit('Unclosed function')


def extract(source):
    return (function(source, 'bool write_exact(SDL_IOStream* stream, const void* src, size_t size) noexcept') +
            function(source, 'bool copy_stream(SDL_IOStream* src, SDL_IOStream* dst) noexcept'))


out = Path(args.output_dir)
out.mkdir(parents=True, exist_ok=True)
original = (aurora / 'lib/io.cpp').read_bytes().decode()
with tempfile.TemporaryDirectory() as work:
    (Path(work) / 'lib').mkdir()
    shutil.copyfile(aurora / 'lib/io.cpp', Path(work) / 'lib/io.cpp')
    subprocess.run(['patch', '-s', '-p1', '--no-backup-if-mismatch', '-d', work, '-i', str(patch)], check=True)
    candidate = (Path(work) / 'lib/io.cpp').read_bytes().decode()
if candidate == original:
    raise SystemExit('Patch 0005 changed nothing')
(out / 'original.inc').write_text('// Generated from pinned Aurora lib/io.cpp; do not edit.\n' + extract(original))
(out / 'candidate.inc').write_text('// Generated from lib/io.cpp + patch 0005; do not edit.\n' + extract(candidate))
