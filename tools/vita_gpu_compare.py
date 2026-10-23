"""Compare two Vita runs at the GPU interface (docs/PERFORMANCE.md, "Comparing renderers").

    python tools/vita_gpu_compare.py digests RUN_A/runtime.jsonl RUN_B/runtime.jsonl [--from N]
    python tools/vita_gpu_compare.py match-list RUN/runtime.jsonl OUT.txt
    python tools/vita_gpu_compare.py pixels CAPTURES_A CAPTURES_B [--diff-dir DIR]

digests: per-frame draw digests (gfx_digest receipts). Reports the frame offset that
lines the two runs up best, how many frames then agree, and the first disagreement.
match-list: the digests of the frames a run captured, for --vita-capture-match.
pixels: pairs PNG captures by digest (file names f<frame>-<digest>.png) and reports,
per pair, differing pixels and the largest channel difference (RGB and alpha apart).
Standard library only; the PNG reader covers the 8-bit RGBA files the game writes.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import zlib
from pathlib import Path

DIGEST = re.compile(r'f=(\d+) d=([0-9a-f]{16}) n=(\d+) c=(\d+) v=(\d+) i=(\d+)')
CAPTURE = re.compile(r'f(\d{6})-([0-9a-f]{16})\.png$')


def read_digests(path: Path) -> dict[int, tuple[str, int]]:
    frames = {}
    for line in path.read_text(errors='replace').splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if row.get('event') != 'gfx_digest':
            continue
        found = DIGEST.match(row.get('text', ''))
        if found:
            frames[int(found.group(1))] = (found.group(2), int(found.group(3)))
    return frames


def best_alignment(a: dict, b: dict, start: int = 1, window: int = 600) -> dict:
    """Offset k (frame f of A ~ frame f+k of B) maximising equal digests from `start`."""
    best = {'offset': 0, 'equal': -1, 'compared': 0}
    keys = sorted(f for f in a if f >= start)
    for k in range(-window, window + 1):
        compared = equal = 0
        for f in keys:
            other = b.get(f + k)
            if other is None:
                continue
            compared += 1
            equal += a[f][0] == other[0]
        if compared and (equal > best['equal'] or (equal == best['equal'] and abs(k) < abs(best['offset']))):
            best = {'offset': k, 'equal': equal, 'compared': compared}
    first_diff = None
    for f in keys:
        other = b.get(f + best['offset'])
        if other is not None and other[0] != a[f][0]:
            first_diff = f
            break
    best['first_difference_frame_a'] = first_diff
    return best


def compare_digests(path_a: Path, path_b: Path, start: int) -> dict:
    a, b = read_digests(path_a), read_digests(path_b)
    same_index = [f for f in sorted(a) if f >= start and f in b]
    equal_same = sum(a[f][0] == b[f][0] for f in same_index)
    first = next((f for f in same_index if a[f][0] != b[f][0]), None)
    return {
        'frames_a': len(a), 'frames_b': len(b), 'from_frame': start,
        'same_index_compared': len(same_index), 'same_index_equal': equal_same,
        'same_index_first_difference': first,
        'same_index_draws_at_first_difference': None if first is None else [a[first][1], b[first][1]],
        'best_alignment': best_alignment(a, b, start),
    }


def match_list(runtime: Path, out: Path) -> int:
    digests = []
    for line in runtime.read_text(errors='replace').splitlines():
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        if row.get('event') == 'frame_capture':
            found = CAPTURE.search(row.get('text', ''))
            if found and found.group(2) != '0' * 16 and found.group(2) not in digests:
                digests.append(found.group(2))
    out.write_bytes(''.join(d + '\n' for d in digests).encode('ascii'))
    return len(digests)


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat, width = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if (depth, colour, interlace) != (8, 6, 0):
                raise ValueError(f'{path}: only 8-bit RGBA, non-interlaced')
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
    raw = zlib.decompress(b''.join(idat))
    stride = width * 4
    out = bytearray(stride * height)
    prev = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for x in range(4, stride):
                line[x] = (line[x] + line[x - 4]) & 255
        elif kind == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif kind == 3:
            for x in range(stride):
                left = line[x - 4] if x >= 4 else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif kind == 4:
            for x in range(stride):
                a = line[x - 4] if x >= 4 else 0
                b = prev[x]
                c = prev[x - 4] if x >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif kind != 0:
            raise ValueError(f'{path}: bad filter {kind}')
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, bytes(out)


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    rows = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def compare_images(a: bytes, b: bytes) -> dict:
    rgb_pixels = alpha_pixels = 0
    rgb_max = alpha_max = 0
    for i in range(0, len(a), 4):
        if a[i:i + 4] == b[i:i + 4]:
            continue
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        if d:
            rgb_pixels += 1
            rgb_max = max(rgb_max, d)
        da = abs(a[i + 3] - b[i + 3])
        if da:
            alpha_pixels += 1
            alpha_max = max(alpha_max, da)
    return {'rgb_differing_pixels': rgb_pixels, 'rgb_max_delta': rgb_max,
            'alpha_differing_pixels': alpha_pixels, 'alpha_max_delta': alpha_max}


def diff_image(a: bytes, b: bytes) -> bytes:
    out = bytearray(len(a))
    for i in range(0, len(a), 4):
        d = max(abs(a[i + k] - b[i + k]) for k in range(3))
        grey = a[i] // 4
        out[i:i + 4] = bytes((255, 0, 255, 255)) if d else bytes((grey, grey, grey, 255))
    return bytes(out)


def compare_pixels(dir_a: Path, dir_b: Path, diff_dir: Path | None) -> dict:
    def index(folder: Path) -> dict[str, Path]:
        found = {}
        for p in sorted(folder.glob('f*.png')):
            m = CAPTURE.search(p.name)
            if m and m.group(2) not in found:
                found[m.group(2)] = p
        return found
    a, b = index(dir_a), index(dir_b)
    pairs = []
    for digest in sorted(set(a) & set(b), key=lambda d: a[d].name):
        wa, ha, pa = read_png(a[digest])
        wb, hb, pb = read_png(b[digest])
        if (wa, ha) != (wb, hb):
            raise ValueError(f'{digest}: size {wa}x{ha} vs {wb}x{hb}')
        result = {'digest': digest, 'a': a[digest].name, 'b': b[digest].name, 'pixels': wa * ha}
        result.update(compare_images(pa, pb))
        if diff_dir and result['rgb_differing_pixels']:
            diff_dir.mkdir(parents=True, exist_ok=True)
            write_png(diff_dir / f'diff-{digest}.png', wa, ha, diff_image(pa, pb))
        pairs.append(result)
    return {'captures_a': len(a), 'captures_b': len(b), 'pairs': pairs,
            'identical_pairs': sum(p['rgb_differing_pixels'] == 0 and p['alpha_differing_pixels'] == 0 for p in pairs)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    d = sub.add_parser('digests')
    d.add_argument('a', type=Path)
    d.add_argument('b', type=Path)
    d.add_argument('--from', dest='start', type=int, default=1)
    m = sub.add_parser('match-list')
    m.add_argument('runtime', type=Path)
    m.add_argument('out', type=Path)
    p = sub.add_parser('pixels')
    p.add_argument('a', type=Path)
    p.add_argument('b', type=Path)
    p.add_argument('--diff-dir', type=Path)
    args = parser.parse_args()
    if args.command == 'digests':
        print(json.dumps(compare_digests(args.a, args.b, args.start), indent=1))
    elif args.command == 'match-list':
        print(match_list(args.runtime, args.out))
    else:
        print(json.dumps(compare_pixels(args.a, args.b, args.diff_dir), indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
