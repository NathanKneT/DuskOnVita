#!/usr/bin/env python3
"""Bench v2 report: per run and phase (idle, walk), where the frame goes and what limits it.

    tools/vita_bench_report.py runtime.jsonl [--walk-start 30] [--settle 10] [--markdown out.md]
                               [--actors] [--json rows.json]

Built on vita_run_report.py: a run is one boot; steady windows are its 30-frame profile reports with
the player present, --settle seconds after the player first appeared, and no shader stage compiled
in the window (compiles are counted apart). Bench v2 runs stand still, then follow a scripted walk:
"idle" is [settle, walk start), "walk" is [walk start, end). The walk starts at the input script's
first step-1 receipt (input_script_step = 1); without one, --walk-start seconds after the player.

Verdict per phase, from per-frame means (ms):
- main thread = game execute + painter, minus the wait for the GX worker when the frame overlap is on
  (its settle sits inside execute); with the overlap off the wait is the frame-end drain, outside both;
- GX worker = FIFO processing; GPU = busy time (only with --vita-gxm-gpu-timing=on).
The limit is the largest of the three; if even that one is under 75 % of the frame time, the frame is
lost to synchronisation or pacing rather than to one thread's work. Shader compiles in play are
reported per run (they are excluded from the steady windows). Nothing is estimated.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vita_run_report as rr  # noqa: E402

TICK_PARTS = {'prof_tick_execute_inclusive_us': 'actors_execute', 'prof_tick_draw_inclusive_us': 'actors_draw',
              'prof_tick_shadow_collect_us': 'shadow_collect', 'prof_tick_particle3d_us': 'particles',
              'prof_tick_demo_us': 'cutscene'}
BUSY_SHARE = 0.75


def overlap_on(switch_text):
    """frame_overlap_requested receipt text -> whether the next tick ran during the last frame's drawing."""
    return switch_text in ('on', 'default on (play profile)')


def main_work(costs, overlap):
    """Main thread work per frame (ms): execute + painter, less the settle wait inside execute (overlap)."""
    work = costs.get('exec_ms', 0.0) + costs.get('painter_ms', 0.0)
    return max(0.0, work - (costs.get('gx_wait_ms', 0.0) if overlap else 0.0))


def verdict(costs, fps, overlap=False):
    """(limit, busiest ms, frame ms) from a phase's per-frame costs and FPS."""
    if not fps:
        return 'no frames', 0.0, 0.0
    frame = 1000.0 / fps
    parts = {'main thread': main_work(costs, overlap), 'GX worker': costs.get('fifo_ms', 0.0)}
    if costs.get('gpu_busy_ms') is not None:
        parts['GPU'] = costs['gpu_busy_ms']
    name, busiest = max(parts.items(), key=lambda kv: kv[1])
    if busiest < BUSY_SHARE * frame:
        return f'sync/pacing (busiest: {name})', busiest, frame
    return name, busiest, frame


def tick_parts(boot, t0, t1):
    """Mean per-frame update parts (ms) over the profile windows whose report time is in [t0, t1)."""
    sums, windows, current = {}, 0, None
    for _, r in boot['records']:
        name, ms = r.get('event'), r.get('ms', 0)
        if name == 'prof_frame_samples':
            current = t0 <= ms < t1
            windows += bool(current)
        elif current and name in TICK_PARTS and isinstance(r.get('value'), (int, float)):
            sums[TICK_PARTS[name]] = sums.get(TICK_PARTS[name], 0) + r['value']
    return {k: round(v / windows / 1000.0, 2) for k, v in sums.items()} if windows else {}


ACTPROF_COUNTS = ('frames', 'frames_0tick', 'frames_1tick', 'frames_2tick', 'ticks_final', 'ticks_nonfinal',
                  'tick_final_exec_us', 'tick_final_draw_us', 'tick_nonfinal_exec_us', 'tick_nonfinal_draw_us',
                  'present_us', 'actor_frames', 'actor_ticks', 'actor_exec_us', 'actor_draw_us',
                  'nonactor_draw_self_us', 'handler_exec_us', 'handler_draw_us', 'drew', 'culled_frustum',
                  'culled_far', 'skipped', 'j3d_frames', 'j3d_ticks', 'j3d_exec_us', 'j3d_draw_final_us',
                  'j3d_draw_final_ticks', 'j3d_draw_nonfinal_us', 'j3d_draw_nonfinal_ticks', 'painter_frames',
                  'painter_us', 'painter_j3d_us', 'painter_j3d_calls', 'reads')
J3D_PARTS = ('calc', 'material', 'diff', 'entry', 'view')
DIST_BINS = ('<1000', '1000-3000', '3000-6000', '>=6000')


def _fields(text):
    return dict(kv.split('=', 1) for kv in text.split(';') if '=' in kv)


def _ints(text):
    return [int(v) for v in text.split(',')]


def actor_profile(boot, t0, t1):
    """Main-thread actor profile windows (runtime/actor_profile.cpp) reported in [t0, t1) ms, summed.

    Per actor frame (one frame in eight): execute and draw time per process profile (top 12 per window,
    so a type outside a window's top 12 is missing from that window), culling and the camera-distance
    histogram of actors that drew. Per J3D frame (another one in eight): J3DModel part time by where it
    ran. Every frame: each tick's execute and draw handler time, final tick vs earlier ticks of the frame.
    Returns None without windows."""
    total = {k: 0 for k in ACTPROF_COUNTS}
    types, dist_n, dist_us = {}, [0] * 4, [0] * 4
    parts = {}
    windows, current, clock_ns = 0, False, None
    for _, r in boot['records']:
        name, ms = r.get('event', ''), r.get('ms', 0)
        if not name.startswith('actprof_'):
            continue
        key = name[len('actprof_'):]
        if key == 'window':
            current = t0 <= ms < t1
            windows += bool(current)
            continue
        if not current:
            continue
        if key in total and isinstance(r.get('value'), (int, float)):
            total[key] += r['value']
        elif key == 'clock_ns':
            clock_ns = r.get('value')
        elif key == 'type' and 'text' in r:
            f = _fields(r['text'])
            t = types.setdefault((f['n'], int(f['p'])), {k: 0 for k in ('ex', 'dw', 'ce', 'cd', 'cf', 'cr', 'cs')})
            for k in ('ex', 'dw', 'ce', 'cd', 'cf', 'cr', 'cs'):
                t[k] += int(f.get(k, 0))
            t['actor'] = f.get('a') == '1'
        elif key == 'dist' and 'text' in r:
            f = _fields(r['text'])
            dist_n = [a + b for a, b in zip(dist_n, _ints(f['n']))]
            dist_us = [a + b for a, b in zip(dist_us, _ints(f['us']))]
        elif key == 'j3d' and 'text' in r:
            f = _fields(r['text'])
            p = parts.setdefault(f['w'], {'us': [0] * 5, 'n': [0] * 5})
            p['us'] = [a + b for a, b in zip(p['us'], _ints(f['us']))]
            p['n'] = [a + b for a, b in zip(p['n'], _ints(f['n']))]
    if not windows:
        return None

    def per(v, n):
        return round(v / n / 1000.0, 3) if n else None

    def per_tick(v):
        return round(v / total['actor_ticks'], 1) if total['actor_ticks'] else None

    af, jf = total['actor_frames'], total['j3d_frames']
    ranked = sorted(types.items(), key=lambda kv: -(kv[1]['ex'] + kv[1]['dw']))
    out = {
        'windows': windows, 'frames': total['frames'], 'clock_ns': clock_ns,
        'frames_by_ticks': [total['frames_0tick'], total['frames_1tick'], total['frames_2tick']],
        # Every frame: ms per tick.
        'tick_final_exec_ms': per(total['tick_final_exec_us'], total['ticks_final']),
        'tick_final_draw_ms': per(total['tick_final_draw_us'], total['ticks_final']),
        'tick_nonfinal_exec_ms': per(total['tick_nonfinal_exec_us'], total['ticks_nonfinal']),
        'tick_nonfinal_draw_ms': per(total['tick_nonfinal_draw_us'], total['ticks_nonfinal']),
        'present_ms': per(total['present_us'], total['frames']),
        # Actor frames: ms per sampled frame.
        'actor_frames': af, 'actor_ticks': total['actor_ticks'],
        'exec_ms': per(total['actor_exec_us'], af), 'draw_ms': per(total['actor_draw_us'], af),
        'handler_exec_ms': per(total['handler_exec_us'], af), 'handler_draw_ms': per(total['handler_draw_us'], af),
        'nonactor_draw_self_ms': per(total['nonactor_draw_self_us'], af),
        'per_tick': {k: per_tick(total[k]) for k in ('drew', 'culled_frustum', 'culled_far', 'skipped')},
        'types': [{'name': n, 'profile': p, 'actor': t.get('actor', False),
                   'exec_ms': per(t['ex'], af), 'draw_ms': per(t['dw'], af),
                   'executed_per_tick': per_tick(t['ce']), 'drew_per_tick': per_tick(t['cd']),
                   'culled_frustum': t['cf'], 'culled_far': t['cr'], 'skipped': t['cs']}
                  for (n, p), t in ranked],
        'distance': [{'bin': b, 'drew_per_tick': per_tick(n), 'draw_ms': per(us, af)}
                     for b, n, us in zip(DIST_BINS, dist_n, dist_us)],
        # J3D frames: ms per sampled frame by where the part ran.
        'j3d_frames': jf, 'j3d_ticks': total['j3d_ticks'],
        'j3d_exec_ms': per(total['j3d_exec_us'], jf),
        'j3d_draw_final_ms': per(total['j3d_draw_final_us'], total['j3d_draw_final_ticks']),
        'j3d_draw_nonfinal_ms': per(total['j3d_draw_nonfinal_us'], total['j3d_draw_nonfinal_ticks']),
        'j3d_draw_final_ticks': total['j3d_draw_final_ticks'],
        'j3d_draw_nonfinal_ticks': total['j3d_draw_nonfinal_ticks'],
        'j3d': {w: {'ms': dict(zip(J3D_PARTS, (per(u, jf) for u in p['us']))),
                    'calls': dict(zip(J3D_PARTS, (round(c / jf, 1) if jf else None for c in p['n'])))}
                for w, p in parts.items()},
        'painter_ms': per(total['painter_us'], total['painter_frames']),
        'painter_j3d_ms': per(total['painter_j3d_us'], total['painter_frames']),
        'reads_per_frame': round(total['reads'] / (af + jf), 1) if af + jf else None,
    }
    # Draw-phase J3D per tick (the frame's final tick vs earlier ticks), not per frame.
    for w, ticks in (('draw_final', total['j3d_draw_final_ticks']),
                     ('draw_nonfinal', total['j3d_draw_nonfinal_ticks'])):
        if w in parts:
            out['j3d'][w]['ms_per_tick'] = dict(zip(J3D_PARTS, (per(u, ticks) for u in parts[w]['us'])))
    return out


def phase(boot, settle, start_s, end_s):
    """vita_run_report's summary of the windows between start_s and end_s after the player appears."""
    first = rr.summarize(boot, settle)[0].get('player_first_ms')
    if first is None:
        return None, None
    end_ms = first + end_s * 1000 if end_s is not None else None
    records = [(n, r) for n, r in boot['records'] if end_ms is None or r.get('ms', 0) < end_ms]
    results = [r for r in rr.summarize({'first_line': boot['first_line'], 'records': records}, start_s)
               if r.get('windows')]
    if not results:
        return None, first
    return max(results, key=lambda r: r['windows']), first


def walk_start_s(boot, player_first_ms, default):
    """Seconds after the player appears at which the input script's walk began: the first
    input_script_step receipt with value 1 (bench scripts hold still in step 0), else the default."""
    if player_first_ms is None:
        return default
    for _, r in boot['records']:
        if r.get('event') == 'input_script_step' and r.get('value') == 1:
            return max(0.0, (r.get('ms', 0) - player_first_ms) / 1000.0)
    return default


def bench(lines, settle, walk_start):
    rows = []
    for boot in rr.split_boots(lines):
        whole = rr.summarize(boot, settle)[0]
        walk = walk_start_s(boot, whole.get('player_first_ms'), walk_start)
        for name, (lo, hi) in (('idle', (settle, walk)), ('walk', (walk, None))):
            run, first = phase(boot, lo, lo, hi)
            if run is None:
                continue
            costs = run.get('costs_ms', {})
            overlap = overlap_on((whole.get('switches') or {}).get('frame_overlap'))
            limit, busiest, frame = verdict(costs, run.get('fps'), overlap)
            t0 = first + lo * 1000
            t1 = first + hi * 1000 if hi is not None else float('inf')
            rows.append({
                'label': whole.get('label') or '-', 'scene': run.get('scene', ''), 'phase': name,
                'fps': run.get('fps'), 'p95_ms': run.get('frame_ms_p95_bin'), 'ticks_per_s': run.get('ticks_per_s'),
                'draws': run.get('draws_per_frame'), 'windows': run.get('windows'),
                'overlap': overlap, 'main_ms': round(main_work(costs, overlap), 2),
                'worker_ms': costs.get('fifo_ms'), 'gpu_ms': costs.get('gpu_busy_ms'),
                'wait_ms': costs.get('gx_wait_ms'), 'frame_ms': round(frame, 2), 'limit': limit,
                'tick': tick_parts(boot, t0, t1),
                'worker_parts': {k: v['ms'] for k, v in run.get('decode', {}).get('classes', {}).items()},
                'compiles': whole.get('shader_play_slow_compiles'),
                'compile_max_ms': round((whole.get('shader_play_compile_max_us') or 0) / 1000.0, 1),
                'actors': actor_profile(boot, t0, t1),
            })
    return rows


def actor_markdown(rows, top=10):
    """Per run and phase: top actor types, culling, distance and the J3D split (actor_profile)."""
    out = []
    for r in rows:
        a = r.get('actors')
        if not a:
            continue
        out.append(f"### {r['label']} {r['phase']} ({a['actor_frames']} actor frames, {a['j3d_frames']} J3D frames, "
                   f"{a['windows']} windows)")
        out.append(f"- ticks per frame (0/1/2+): {a['frames_by_ticks']}; per tick ms: final exec "
                   f"{fmt(a['tick_final_exec_ms'])} draw {fmt(a['tick_final_draw_ms'])}, earlier tick exec "
                   f"{fmt(a['tick_nonfinal_exec_ms'])} draw {fmt(a['tick_nonfinal_draw_ms'])}; presentation pass "
                   f"{fmt(a['present_ms'])} ms/frame")
        out.append(f"- actor frames: execute {fmt(a['exec_ms'])} ms, draw {fmt(a['draw_ms'])} ms "
                   f"(non-actor draw self {fmt(a['nonactor_draw_self_ms'])}); per tick {a['per_tick']}")
        out.append('| Type | profile | exec ms | draw ms | exec/tick | drew/tick | culled frustum | culled far | skipped |')
        out.append('|---|---:|---:|---:|---:|---:|---:|---:|---:|')
        for t in a['types'][:top]:
            out.append(f"| {t['name']} | {t['profile']} | {fmt(t['exec_ms'])} | {fmt(t['draw_ms'])} | "
                       f"{fmt(t['executed_per_tick'])} | {fmt(t['drew_per_tick'])} | {t['culled_frustum']} | "
                       f"{t['culled_far']} | {t['skipped']} |")
        out.append('- distance of actors that drew: ' + ', '.join(
            f"{d['bin']}: {fmt(d['drew_per_tick'])}/tick {fmt(d['draw_ms'])} ms" for d in a['distance']))
        for w, p in sorted(a['j3d'].items()):
            if any(p['calls'].values()):
                out.append(f"- J3D {w} ms/frame: " + ', '.join(f"{k} {fmt(v)}" for k, v in p['ms'].items())
                           + (('; per tick ' + ', '.join(f"{k} {fmt(v)}" for k, v in p['ms_per_tick'].items()))
                              if 'ms_per_tick' in p else ''))
        out.append(f"- J3D frames: draw handler per tick final {fmt(a['j3d_draw_final_ms'])} ms "
                   f"({a['j3d_draw_final_ticks']} ticks), earlier {fmt(a['j3d_draw_nonfinal_ms'])} ms "
                   f"({a['j3d_draw_nonfinal_ticks']} ticks); painter {fmt(a['painter_ms'])} ms of which J3D draw "
                   f"buffers {fmt(a['painter_j3d_ms'])}; probe clock reads/sampled frame {fmt(a['reads_per_frame'])} "
                   f"at {fmt(a['clock_ns'])} ns")
        out.append('')
    return '\n'.join(out) + ('\n' if out else '')


def fmt(v):
    return '-' if v is None else (f'{v:.1f}' if isinstance(v, float) else str(v))


def markdown(rows):
    out = ['| Run | Scene | Phase | FPS | p95 ms | ticks/s | draws | main ms | GX worker ms | GPU ms | wait ms | Limit |',
           '|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|']
    for r in rows:
        out.append('| ' + ' | '.join(fmt(r[k]) for k in ('label', 'scene', 'phase', 'fps', 'p95_ms', 'ticks_per_s',
                                                        'draws', 'main_ms', 'worker_ms', 'gpu_ms', 'wait_ms', 'limit'))
                   + ' |')
    out.append('')
    for r in rows:
        top = sorted(r['worker_parts'].items(), key=lambda kv: -kv[1])[:3]
        tick = ', '.join(f'{k} {v}' for k, v in sorted(r['tick'].items(), key=lambda kv: -kv[1])[:3])
        out.append(f"- {r['label']} {r['phase']}: GX worker top "
                   + (', '.join(f'{k} {v}' for k, v in top) or '-')
                   + f"; update parts {tick or '-'}; shader compiles in play {fmt(r['compiles'])}"
                   + (f" (worst {r['compile_max_ms']} ms)" if r['compiles'] else ''))
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('receipt', type=Path)
    ap.add_argument('--settle', type=float, default=10.0)
    ap.add_argument('--walk-start', type=float, default=30.0, help='seconds after the player appears')
    ap.add_argument('--markdown', type=Path)
    ap.add_argument('--actors', action='store_true', help='add the main-thread actor profile per run and phase')
    ap.add_argument('--json', type=Path, help='write the rows (with the actor profile) as JSON')
    a = ap.parse_args()
    rows = bench(a.receipt.read_bytes().decode('utf-8', 'replace').splitlines(), a.settle, a.walk_start)
    text = markdown(rows)
    if a.actors:
        text += '\n' + actor_markdown(rows)
    if a.json:
        a.json.write_text(json.dumps(rows, indent=1), encoding='utf-8')
    if a.markdown:
        a.markdown.write_text(text, encoding='utf-8')
    sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
