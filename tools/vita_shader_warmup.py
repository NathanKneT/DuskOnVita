"""Build a bounded private Cg source bundle from explicit runtime capture directories."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

MAGIC = b'TWVCGB01'
MAX_SOURCE_BYTES = 4 * 1024 * 1024
MAX_STAGES = 1024
MAX_BUNDLE_BYTES = 24 + MAX_SOURCE_BYTES + MAX_STAGES * 9


def validate_bundle(data: bytes) -> list[tuple[int, bytes]]:
    if not 24 <= len(data) <= MAX_BUNDLE_BYTES:
        raise ValueError('bundle size outside bounds')
    magic, version, count, size, crc = struct.unpack_from('<8sIIII', data)
    if (magic != MAGIC or version != 1 or not 1 <= count <= MAX_STAGES or
            size != len(data)-24 or zlib.crc32(data[24:]) != crc):
        raise ValueError('invalid source bundle header/checksum')
    cursor, total, sources = 24, 0, []
    for _ in range(count):
        if len(data)-cursor < 9:
            raise ValueError('truncated source entry')
        stage, length = struct.unpack_from('<II', data, cursor)
        cursor += 8
        if (stage not in (1, 2) or not 1 <= length <= 65536 or
                length > MAX_SOURCE_BYTES-total or length >= len(data)-cursor):
            raise ValueError('invalid source entry bounds/stage')
        source = data[cursor:cursor+length]
        if data[cursor+length] or b'\0' in source:
            raise ValueError('source requires exactly one final NUL')
        sources.append((stage, source))
        cursor += length+1
        total += length
    if cursor != len(data):
        raise ValueError('trailing source bundle data')
    return sources


def encode_bundle(sources: list[tuple[int, bytes]]) -> bytes:
    if any(type(stage) is not int or stage not in (1, 2) or not 1 <= len(source) <= 65536 or
           b'\0' in source for stage, source in sources):
        raise ValueError('invalid source input')
    unique = sorted(set(sources), key=lambda item: (item[0], hashlib.sha256(item[1]).hexdigest()))
    if not 1 <= len(unique) <= MAX_STAGES:
        raise ValueError(f'bundle requires 1-{MAX_STAGES} unique stages')
    if sum(len(source) for _, source in unique) > MAX_SOURCE_BYTES:
        raise ValueError(f'source budget exceeds {MAX_SOURCE_BYTES} bytes')
    payload = b''.join(struct.pack('<II', stage, len(source))+source+b'\0' for stage, source in unique)
    data = struct.pack('<8sIIII', MAGIC, 1, len(unique), len(payload), zlib.crc32(payload))+payload
    assert validate_bundle(data) == unique
    return data


def collect_sources(roots: list[Path]) -> tuple[list[tuple[int, bytes]], list[dict]]:
    sources, evidence = [], []
    if not 1 <= len(roots) <= 16:
        raise ValueError('explicit 1–16 capture directories required')
    for root in roots:
        if not root.is_absolute() or root.is_symlink() or not root.is_dir():
            raise ValueError('source root must be an existing absolute capture directory')
        root = root.resolve()
        files = sorted(root.glob('*.cg'))
        if not 1 <= len(files) <= MAX_STAGES:
            raise ValueError(f'each capture must have 1-{MAX_STAGES} source files')
        for path in files:
            match = re.fullmatch(r'[0-9]{3,4}-([vf])\.cg', path.name)
            if not match or path.is_symlink() or not path.resolve().is_relative_to(root) or not path.is_file():
                raise ValueError('invalid source capture member')
            if not 1 <= path.stat().st_size <= 65536:
                raise ValueError('source size outside bounds')
            source = path.read_bytes()
            if not 1 <= len(source) <= 65536 or b'\0' in source:
                raise ValueError('invalid captured source bytes')
            stage = 1 if match[1] == 'v' else 2
            sources.append((stage, source))
            evidence.append({'path': str(path), 'stage': stage, 'bytes': len(source),
                             'sha256': hashlib.sha256(source).hexdigest()})
    return sources, evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    build = sub.add_parser('build')
    build.add_argument('--sources', type=Path, action='append', required=True)
    build.add_argument('--output', type=Path, required=True)
    build.add_argument('--manifest', type=Path, required=True)
    build.add_argument('--source-commit', required=True)
    build.add_argument('--producer-vpk-sha256', required=True)
    build.add_argument('--compiler-sha256', required=True)
    check = sub.add_parser('inspect')
    check.add_argument('bundle', type=Path)
    args = parser.parse_args()
    if args.command == 'inspect':
        if not 24 <= args.bundle.stat().st_size <= MAX_BUNDLE_BYTES:
            parser.error('bundle outside bounds')
        data = args.bundle.read_bytes(); stages = validate_bundle(data)
        print(json.dumps({'stages': len(stages), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}))
        return 0
    if args.output == args.manifest or args.output.exists() or args.manifest.exists():
        parser.error('choose two new output paths; inputs/evidence are never overwritten')
    if (not re.fullmatch(r'[a-f0-9]{40}', args.source_commit) or
            any(not re.fullmatch(r'[a-f0-9]{64}', digest) for digest in
                (args.producer_vpk_sha256, args.compiler_sha256))):
        parser.error('exact source commit and producer/compiler SHA-256 required')
    sources, evidence = collect_sources(args.sources)
    data = encode_bundle(sources)
    record = {'format': 1, 'source_commit': args.source_commit,
        'producer_vpk_sha256': args.producer_vpk_sha256, 'compiler_sha256': args.compiler_sha256,
        'bundle': {'path': str(args.output.resolve()), 'bytes': len(data),
                   'sha256': hashlib.sha256(data).hexdigest(), 'stages': len(validate_bundle(data))},
        'sources': evidence,
        'scope': 'Recorded generated Cg sources only. First-use preparation time remains startup work. Unseen sources still compile normally. No console, coverage, rendering or FPS certification.'}
    with args.output.open('xb') as f:
        f.write(data)
    with args.manifest.open('x') as f:
        json.dump(record, f, indent=2); f.write('\n')
    print(json.dumps(record['bundle'], indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
