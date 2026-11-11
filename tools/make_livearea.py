#!/usr/bin/env python3
"""Draws the LiveArea pictures of the VPK (platform/vita/sce_sys): icon, background, gate image.

Everything is drawn here in code: a dusk sky, a low sun, two lines of hills and the app name in a
5x7 pixel font. No game, Nintendo or Sony artwork, and no file is read. The Vita only installs
LiveArea pictures that are 8-bit palette PNGs, so each picture is reduced to 256 colours.

  python3 tools/make_livearea.py     writes the three PNGs

  icon0.png                          128x128   the bubble on the home screen
  livearea/contents/bg.png           840x500   the LiveArea page
  livearea/contents/startup.png      280x158   the gate (the picture you press to start)
Python 3 standard library only.
"""
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "platform" / "vita" / "sce_sys"

FONT = {
    "D": ("####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."),
    "O": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "V": ("#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "a": (".....", ".....", ".###.", "....#", ".####", "#...#", ".####"),
    "i": ("..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."),
    "k": ("#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."),
    "n": (".....", ".....", "#.##.", "##..#", "#...#", "#...#", "#...#"),
    "s": (".....", ".....", ".####", "#....", ".###.", "....#", "####."),
    "t": (".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##."),
    "u": (".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#"),
}

SKY = ((0.00, (14, 18, 44)), (0.45, (58, 44, 96)), (0.78, (168, 84, 96)), (1.00, (244, 150, 78)))
SUN = (255, 222, 164)
FAR_HILLS = (44, 34, 66)
NEAR_HILLS = (12, 13, 22)
INK = (246, 240, 228)
SHADE = (10, 10, 18)


def mix(a, b, t):
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(3))


def sky(v):
    """Sky colour at height v (0 top, 1 horizon)."""
    v = min(max(v, 0.0), 1.0)
    for (v0, c0), (v1, c1) in zip(SKY, SKY[1:]):
        if v <= v1:
            return mix(c0, c1, (v - v0) / (v1 - v0))
    return SKY[-1][1]


def scene(width, height, horizon):
    """The picture as rows of float RGB. horizon: the near hills' base line, 0..1 of the height."""
    base = horizon * height
    sun_x, sun_y, sun_r = width * 0.5, base - height * 0.02, min(width, height) * 0.17
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            u = x / width
            colour = sky(y / base)
            # The sun: a soft glow, then the disc with one pixel of smoothing.
            d = math.hypot(x + 0.5 - sun_x, y + 0.5 - sun_y)
            colour = mix(colour, SUN, 0.55 * math.exp(-((d / (sun_r * 2.4)) ** 2)))
            colour = mix(colour, SUN, min(max(sun_r - d + 0.5, 0.0), 1.0))
            far = base - height * (0.10 + 0.05 * math.sin(u * 7.1 + 0.6) + 0.03 * math.sin(u * 17.3 + 2.0))
            colour = mix(colour, FAR_HILLS, min(max(y + 0.5 - far, 0.0), 1.0))
            near = base - height * (0.02 + 0.045 * math.sin(u * 4.3 + 3.9) + 0.015 * math.sin(u * 11.9 + 1.1))
            colour = mix(colour, NEAR_HILLS, min(max(y + 0.5 - near, 0.0), 1.0))
            row.append(colour)
        rows.append(row)
    return rows


def text_width(text, scale):
    return (len(text) * 6 - 1) * scale


def draw_text(rows, text, x0, y0, scale):
    """The text with a one-pixel dark edge under it, top-left corner at (x0, y0)."""
    cells = [(x0 + (i * 6 + k) * scale, y0 + r * scale)
             for i, ch in enumerate(text) for r, line in enumerate(FONT[ch]) for k, c in enumerate(line) if c == "#"]
    for colour, grow in ((SHADE, 1), (INK, 0)):
        for cx, cy in cells:
            for y in range(cy - grow, cy + scale + grow):
                for x in range(cx - grow, cx + scale + grow):
                    if 0 <= y < len(rows) and 0 <= x < len(rows[0]):
                        rows[y][x] = colour


BAYER = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)


def palette_png(rows):
    """8-bit palette PNG (256 colours at most) of float RGB rows: median cut over a 6-bit histogram,
    with a small ordered dither so the sky does not band."""
    height, width = len(rows), len(rows[0])
    keys = []
    counts = {}
    for y, row in enumerate(rows):
        line = []
        for x, colour in enumerate(row):
            noise = (BAYER[(y % 4) * 4 + x % 4] + 0.5) / 16.0 - 0.5
            key = tuple(min(63, max(0, int((c + noise * 4.0) / 4.0))) for c in colour)
            counts[key] = counts.get(key, 0) + 1
            line.append(key)
        keys.append(line)
    boxes = [list(counts)]
    while len(boxes) < 256:
        widest, axis, span = -1, 0, 0
        for i, box in enumerate(boxes):
            for k in range(3):
                values = [c[k] for c in box]
                if max(values) - min(values) > span:
                    widest, axis, span = i, k, max(values) - min(values)
        if widest < 0:
            break
        box = sorted(boxes[widest], key=lambda c: (c[axis], c))
        half, run, cut = sum(counts[c] for c in box) / 2.0, 0, 1
        for i, c in enumerate(box[:-1]):
            run += counts[c]
            cut = i + 1
            if run >= half:
                break
        boxes[widest:widest + 1] = [box[:cut], box[cut:]]
    palette = bytearray()
    index = {}
    for i, box in enumerate(boxes):
        total = sum(counts[c] for c in box)
        for k in range(3):
            palette.append(min(255, round(sum(c[k] * counts[c] for c in box) / total * 4.0 + 2.0)))
        for c in box:
            index[c] = i
    raw = bytearray()
    for line in keys:
        raw.append(0)
        raw.extend(index[key] for key in line)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)) +
            chunk(b"PLTE", bytes(palette)) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def pictures():
    icon = scene(128, 128, 0.62)
    draw_text(icon, "Dusk", (128 - text_width("Dusk", 4)) // 2, 76, 4)
    draw_text(icon, "OnVita", (128 - text_width("OnVita", 2)) // 2, 108, 2)
    background = scene(840, 500, 0.86)
    gate = scene(280, 158, 0.60)
    draw_text(gate, "DuskOnVita", (280 - text_width("DuskOnVita", 4)) // 2, 106, 4)
    return {"icon0.png": icon, "livearea/contents/bg.png": background, "livearea/contents/startup.png": gate}


def main():
    if sys.argv[1:]:
        sys.exit(__doc__)
    for name, rows in pictures().items():
        data = palette_png(rows)
        path = ROOT / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"{name}: {len(rows[0])}x{len(rows)}, {len(data)} bytes")


if __name__ == "__main__":
    main()
