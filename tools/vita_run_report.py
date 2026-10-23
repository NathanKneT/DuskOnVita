#!/usr/bin/env python3
"""Per-run summary of a console runtime.jsonl, and benchmark-ledger rows.

    tools/vita_run_report.py runtime.jsonl [--json out.json]
        [--ledger bench_ledger.csv --version t1 --commit abc1234 --receipt-path P]

A run is one boot (the "boot" event). Its steady windows are the 30-frame
profile reports taken with the player present, at least --settle seconds after
the player first appeared, with no shader stage compiled during the window.
FPS is frame-weighted over those windows; ticks/s comes from the original
game's execute counter over the same span. Nothing is estimated: a value that
the log does not contain stays empty.
"""
import argparse
import csv
import datetime
import hashlib
import json
import sys
from pathlib import Path

SCENES = {'R_SP01': 'house-R_SP01', 'F_SP103': 'outside-F_SP103', 'F_SP108': 'faron-F_SP108'}
INCLUSIVE = {'prof_game_execute_inclusive_us': 'exec_ms', 'prof_painter_inclusive_us': 'painter_ms',
             'prof_fifo_inclusive_us': 'fifo_ms', 'prof_shader_us': 'shader_ms',
             'prof_present_us': 'present_ms', 'prof_game_audio_us': 'audio_ms', 'prof_events_us': 'events_ms'}
SWITCHES = ['vertex_stream_requested', 'small_packets_requested', 'uncached_staging_requested',
            'gx_decode_requested', 'lazy_vertex_plan_requested', 'geometry_neon_requested',
            'vertex_cull_requested', 'sparse_spans_requested', 'shadow_image_skip_requested',
            'static_geometry_requested', 'gx_thread_requested', 'neon_decode_requested']


def split_boots(lines):
    boots, current = [], None
    for number, line in enumerate(lines, 1):
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        if record.get('event') == 'boot' and record.get('ms') == 0:
            current = {'first_line': number, 'records': []}
            boots.append(current)
        if current is not None:
            current['records'].append((number, record))
    return boots


def hist_p95(bounds, counts):
    total = sum(counts)
    if not total:
        return None
    seen = 0
    for i, count in enumerate(counts):
        seen += count
        if seen >= 0.95 * total:
            return bounds[i] if i < len(bounds) else None
    return None


def summarize(boot, settle_s):
    text, value, value_last = {}, {}, {}
    stage, player, player_first_ms = '', 0, None
    compile_calls = None
    draws = frames_presented = None
    reports, sims, draw_samples = [], [], []
    report = None
    bounds = None
    for number, r in boot['records']:
        name, ms = r.get('event'), r.get('ms', 0)
        if 'text' in r:
            text.setdefault(name, r['text'])
            if name == 'stage_name':
                stage = r['text']
            if name == 'prof_interval_hist_bounds_us':
                bounds = [int(x) for x in r['text'].split(',') if x.strip().lstrip('-').isdigit()]
            if name == 'prof_interval_hist_counts' and report is not None:
                report['hist'] = [int(x) for x in r['text'].split(',') if x.strip().isdigit()]
            continue
        v = r.get('value')
        value.setdefault(name, v)
        value_last[name] = v
        if name == 'player_exists':
            player = v
            if v and player_first_ms is None:
                player_first_ms = ms
        elif name == 'prof_frame_samples':
            report = {'line': number, 'ms': ms, 'frames': v, 'stage': stage, 'player': player,
                      'compile_before': compile_calls, 'costs': {}}
            reports.append(report)
        elif report is not None and name in ('prof_interval_samples', 'prof_frame_interval_us',
                                             'prof_frame_p95_us', 'prof_frame_max_us', 'prof_frame_over_33334us'):
            report[name] = v
        elif report is not None and name in INCLUSIVE:
            report['costs'][name] = v
        elif name == 'gfx_shader_stage_compile_calls':
            compile_calls = v
            if report is not None and 'compile_after' not in report:
                report['compile_after'] = v
        elif name == 'simulation_execute_calls':
            sims.append((ms, v, stage, player))
        elif name == 'gfx_draws':
            draws = v
        elif name == 'gfx_frames_presented':
            frames_presented = v
            if draws is not None:
                draw_samples.append((ms, frames_presented, draws, stage))
    out = {
        'first_line': boot['first_line'], 'label': text.get('sequence_label', ''),
        'sequence_index': value.get('sequence_index'), 'clock_mode': text.get('clock_mode'),
        'clocks_observed': [value.get(f'clock_observed_{a}_mhz_result') for a in ('arm', 'bus', 'gpu', 'gpu_xbar')],
        'switches': {k.replace('_requested', ''): text.get(k) for k in SWITCHES},
        'telemetry_async': value.get('telemetry_async_writer'),
        'player_first_ms': player_first_ms, 'exit_after_s': value.get('run_exit_after_requested_s'),
        'run_exit_seen': any(r.get('event') == 'run_exit_after_s' for _, r in boot['records']),
        'last_ms': boot['records'][-1][1].get('ms', 0) if boot['records'] else 0,
        'reports': len(reports),
        'shader_play_compiles': value_last.get('gfx_shader_play_compiles'),
        'shader_play_slow_compiles': value_last.get('gfx_shader_play_slow_compiles'),
        'shader_play_compile_max_us': value_last.get('gfx_shader_play_compile_max_us'),
        'shader_warmup_prepared': value.get('shader_warmup_prepared'),
    }
    by_stage = {}
    for rep in reports:
        if not rep['player'] or player_first_ms is None or rep['ms'] < player_first_ms + settle_s * 1000:
            continue
        before, after = rep.get('compile_before'), rep.get('compile_after')
        if before is None or after is None or after != before:
            continue
        if not rep.get('prof_interval_samples') or not rep.get('prof_frame_interval_us'):
            continue
        by_stage.setdefault(rep['stage'], []).append(rep)
    if not by_stage:
        out['scene'] = ''
        out['windows'] = 0
        out['status'] = 'no_scene' if out['run_exit_seen'] else 'ended_early'
        return [out]
    results = []
    for scene_stage, steady in by_stage.items():
        results.append(scene_result(dict(out), scene_stage, steady, bounds, sims, draw_samples))
    return results


def scene_result(out, scene_stage, steady, bounds, sims, draw_samples):
    out['scene'] = SCENES.get(scene_stage, scene_stage or '')
    out['windows'] = len(steady)
    intervals = sum(r['prof_interval_samples'] for r in steady)
    total_us = sum(r['prof_interval_samples'] * r['prof_frame_interval_us'] for r in steady)
    out['status'] = 'ok'
    out['frames'] = intervals
    out['fps'] = round(intervals * 1e6 / total_us, 3)
    out['frame_ms_mean'] = round(total_us / intervals / 1000, 2)
    out['frame_ms_max'] = round(max(r.get('prof_frame_max_us', 0) for r in steady) / 1000, 1)
    hist = None
    for r in steady:
        if 'hist' in r:
            hist = r['hist'] if hist is None else [a + b for a, b in zip(hist, r['hist'])]
    p95 = hist_p95(bounds, hist) if bounds and hist else None
    out['frame_ms_p95_bin'] = round(p95 / 1000, 1) if p95 else None
    out['over_33ms'] = sum(r.get('prof_frame_over_33334us', 0) for r in steady)
    frames = sum(r['frames'] for r in steady)
    out['costs_ms'] = {}
    for key, label in INCLUSIVE.items():
        if all(key in r['costs'] for r in steady):
            out['costs_ms'][label] = round(sum(r['costs'][key] * r['frames'] for r in steady) / frames / 1000, 2)
    t0, t1 = steady[0]['ms'], steady[-1]['ms']
    span = [s for s in sims if t0 - 1000 <= s[0] <= t1 + 1000 and s[2] == scene_stage and s[3]]
    if len(span) >= 2 and span[-1][0] > span[0][0]:
        out['ticks_per_s'] = round((span[-1][1] - span[0][1]) * 1000 / (span[-1][0] - span[0][0]), 3)
    ds = [d for d in draw_samples if t0 - 2000 <= d[0] <= t1 + 2000 and d[3] == scene_stage]
    if len(ds) >= 2 and ds[-1][1] > ds[0][1]:
        out['draws_per_frame'] = round((ds[-1][2] - ds[0][2]) / (ds[-1][1] - ds[0][1]), 1)
    return out


def ledger_rows(runs, args, receipt_sha):
    rows = []
    now = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    for run in runs:
        if not run.get('label'):
            continue
        config = [f'clocks={run["clock_mode"]}']
        config += [f'{k}={v.split()[-1] if v else ""}' for k, v in run['switches'].items() if v]
        config.append(f'telemetry_async={run["telemetry_async"]}')
        details = {f'clock_{a}_mhz': c for a, c in zip(('arm', 'bus', 'gpu', 'xbar'), run['clocks_observed'])}
        details.update(run.get('costs_ms', {}))
        if run.get('draws_per_frame') is not None:
            details['draws_per_frame'] = run['draws_per_frame']
        if run.get('frame_ms_p95_bin') is not None:
            details['p95_hist_bin_ms'] = run['frame_ms_p95_bin']
        details['over_33ms'] = run.get('over_33ms')
        for key in ('shader_warmup_prepared', 'shader_play_compiles', 'shader_play_slow_compiles', 'shader_play_compile_max_us'):
            details[key] = run.get(key)
        rows.append({
            'recorded_utc': now, 'owner': '', 'platform': 'vita', 'environment': 'hardware',
            'version': args.version, 'commit': args.commit, 'config': ';'.join(config),
            'scene': run.get('scene', ''), 'status': run['status'],
            'run_ref': f'{run["label"]} @ line {run["first_line"]}', 'windows': run.get('windows', 0),
            'frames': run.get('frames', ''), 'fps': run.get('fps', ''), 'ticks_per_s': run.get('ticks_per_s', ''),
            'frame_ms_mean': run.get('frame_ms_mean', ''), 'frame_ms_max': run.get('frame_ms_max', ''),
            'frame_ms_p95': '', 'kept': '',
            'details': ';'.join(f'{k}={v}' for k, v in details.items() if v is not None),
            'notes': args.notes, 'receipt': args.receipt_path, 'receipt_sha256': receipt_sha,
        })
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('receipt')
    parser.add_argument('--settle', type=float, default=10.0, help='seconds after the player appears')
    parser.add_argument('--json')
    parser.add_argument('--ledger')
    parser.add_argument('--version', default='')
    parser.add_argument('--commit', default='')
    parser.add_argument('--receipt-path', default='')
    parser.add_argument('--notes', default='')
    args = parser.parse_args()
    raw = Path(args.receipt).read_bytes()
    receipt_sha = hashlib.sha256(raw).hexdigest()
    runs = [run for boot in split_boots(raw.decode('utf-8', 'replace').splitlines()) for run in summarize(boot, args.settle)]
    result = {'receipt_sha256': receipt_sha, 'runs': runs}
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1) + '\n')
    for run in runs:
        print(f"{run['first_line']:>7} {run['label'] or '-':<22} {run.get('scene', ''):<16} {run['status']:<11} "
              f"fps={run.get('fps', '')} ticks/s={run.get('ticks_per_s', '')} windows={run.get('windows', 0)} "
              f"clocks={run['clocks_observed']} {run.get('costs_ms', '')}")
    if args.ledger:
        if not args.version or not args.commit or not args.receipt_path:
            sys.exit('--ledger needs --version, --commit and --receipt-path')
        ledger = Path(args.ledger)
        with ledger.open(newline='') as f:
            reader = csv.DictReader(f)
            header = reader.fieldnames
            count = sum(1 for _ in reader)
        rows = ledger_rows(runs, args, receipt_sha)
        with ledger.open('a', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=header, lineterminator='\n')
            for i, row in enumerate(rows, count + 1):
                writer.writerow({'row_id': i, **row})
        print(f'appended {len(rows)} rows to {ledger}')


if __name__ == '__main__':
    main()
