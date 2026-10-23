"""Draw the README charts (plain SVG, no dependencies) from console measurements.

    python3 tools/charts.py   ->  docs/img/progress.svg, docs/img/frame.svg

progress.svg reads docs/benchmarks.csv (one row per console run: add a row, run this script).
"""
import csv
import os

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, '..', 'docs', 'img')
FONT = "font-family='-apple-system,Segoe UI,Helvetica,Arial,sans-serif'"
INK, MUTED, GRID = '#1f2328', '#59636e', '#d1d9e0'
BLUE, GREEN, RED, ORANGE = '#0969da', '#1a7f37', '#cf222e', '#bc4c00'

# Link's house, the GX worker's time per frame on a PS Vita 1000 (ms, build g2c, --vita-gpu=gxm,
# 444 MHz, gfx_decode_probe_* receipts as printed by tools/vita_run_report.py).
FRAME = [('Backend draws', 10.86), ('Pipeline lookup', 8.16), ('XF: matrices, lights', 6.01),
         ('BP: TEV, blend, textures', 5.96), ('Draw parsing', 2.87), ('CP: vertex arrays', 2.29),
         ('Draw setup', 0.81), ('Command loop, other', 0.42), ('Static lists', 0.40)]


def runs():
    """Vita rows by scene: builds in order, FPS per build, rejected builds."""
    with open(os.path.join(HERE, '..', 'docs', 'benchmarks.csv'), newline='', encoding='utf-8') as f:
        rows = [r for r in csv.DictReader(f) if r['console'] == 'vita1000']
    builds, fps, rejected = [], {'house': {}, 'outside': {}}, {}
    for r in rows:
        if r['build'] not in builds:
            builds.append(r['build'])
        if r['scene'] in fps:
            fps[r['scene']][r['build']] = float(r['fps'])
        if r['status'] != 'measured':
            rejected[r['build']] = r['note']
    return builds, fps, rejected


def text(x, y, s, size=13, color=INK, anchor='start', weight='normal'):
    return (f"<text x='{x:.1f}' y='{y:.1f}' {FONT} font-size='{size}' fill='{color}' "
            f"text-anchor='{anchor}' font-weight='{weight}'>{s}</text>")


def progress():
    builds, fps, rejected = runs()
    w, h, left, right, top, bottom = 820, 420, 60, 40, 56, 104
    pw, ph, ymax = w - left - right, h - top - bottom, 32
    xs = {b: left + pw * (i + 0.5) / len(builds) for i, b in enumerate(builds)}
    y = lambda v: top + ph * (1 - v / ymax)
    s = [f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {w} {h}' width='{w}' height='{h}'>",
         f"<rect width='{w}' height='{h}' rx='10' fill='#ffffff' stroke='{GRID}'/>",
         text(left, 28, 'PS Vita 1000 — frames per second, measured on the console', 16, weight='600'),
         text(left, 46, 'every point is a hardware run (docs/benchmarks.csv); emulator numbers are not shown', 12, MUTED)]
    for v in range(0, ymax + 1, 5):
        s.append(f"<line x1='{left}' x2='{w - right}' y1='{y(v):.1f}' y2='{y(v):.1f}' stroke='{GRID}'/>")
        s.append(text(left - 8, y(v) + 4, str(v), 11, MUTED, 'end'))
    s.append(f"<line x1='{left}' x2='{w - right}' y1='{y(30):.1f}' y2='{y(30):.1f}' stroke='{GREEN}' stroke-width='2' stroke-dasharray='6 4'/>")
    s.append(text(w - right, y(30) - 6, 'target: 30', 12, GREEN, 'end', '600'))
    for scene, color, dx in (('house', BLUE, -10), ('outside', ORANGE, 10)):
        series = fps[scene]
        pts = [b for b in builds if b in series]
        for b in pts:
            v, cx = series[b], xs[b] + dx
            fill = '#ffffff' if b in rejected else color
            s.append(f"<circle cx='{cx:.1f}' cy='{y(v):.1f}' r='5' fill='{fill}' stroke='{color}' stroke-width='2'/>")
            s.append(text(cx, y(v) - 10, f'{v:.1f}', 11, color, 'middle', '600'))
    for b in builds:
        s.append(text(xs[b], h - bottom + 18, b, 12, INK, 'middle', '600'))
    ly = h - bottom + 44
    for lx, label, color in ((left, "Link's house", BLUE), (left + 120, 'Ordon, outside', ORANGE)):
        s.append(f"<circle cx='{lx + 6}' cy='{ly}' r='5' fill='{color}'/>")
        s.append(text(lx + 18, ly + 4, label, 11))
    s.append(f"<circle cx='{left + 256}' cy='{ly}' r='5' fill='#ffffff' stroke='{MUTED}' stroke-width='2'/>")
    s.append(text(left + 268, ly + 4, 'run rejected: ' + '; '.join(f'{b}, {why}' for b, why in rejected.items()), 11))
    s.append(text(left, h - 20, 'v61 ran at the system clocks (333 MHz), the other builds at 444 MHz; g2c-gxm uses the experimental GXM renderer.', 11, MUTED))
    s.append('</svg>')
    return '\n'.join(s)


def frame():
    w, left, right, top, row = 760, 200, 70, 64, 30
    h = top + row * len(FRAME) + 40
    total = sum(v for _, v in FRAME)
    vmax = max(v for _, v in FRAME)
    pw = w - left - right
    s = [f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {w} {h}' width='{w}' height='{h}'>",
         f"<rect width='{w}' height='{h}' rx='10' fill='#ffffff' stroke='{GRID}'/>",
         text(24, 28, "Link's house — where the GX worker's time goes (ms per frame, console, GXM)", 16, weight='600'),
         text(24, 46, f'build g2c, decode probe on one frame in 30: {total:.1f} ms; over all frames the FIFO averages 31.5 ms', 12, MUTED)]
    for i, (name, v) in enumerate(FRAME):
        yy = top + i * row
        s.append(text(left - 10, yy + 15, name, 13, INK, 'end'))
        s.append(f"<rect x='{left}' y='{yy}' width='{pw * v / vmax:.1f}' height='20' rx='3' fill='{ORANGE}'/>")
        s.append(text(left + pw * v / vmax + 8, yy + 15, f'{v:.1f}', 12, INK))
    s.append(text(24, h - 14, "all of it is the GX translation layer (Aurora's command processor + the GXM backend) — help wanted", 11, ORANGE))
    s.append('</svg>')
    return '\n'.join(s)


os.makedirs(OUT, exist_ok=True)
for name, svg in (('progress.svg', progress()), ('frame.svg', frame())):
    with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
        f.write(svg + '\n')
    print('wrote', name)
