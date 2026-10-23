"""Read bounded CPU attribution receipts; never infer GPU time or console FPS."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def write_json(path: Path, value: dict) -> None:
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def parse_receipts(path: Path) -> tuple[list[dict], list[str]]:
    rows, errors = [], []
    if not path.is_file():
        return rows, ['guest receipt missing']
    for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
        try:
            row = json.loads(line)
            if not isinstance(row, dict) or not isinstance(row.get('event'), str) or type(row.get('ms')) is not int or row['ms'] < 0:
                raise ValueError('event and nonnegative integer timestamp required')
            if rows and row['ms'] < rows[-1]['ms']:
                raise ValueError('timestamp regressed')
            rows.append(row)
        except (ValueError, TypeError) as error:
            errors.append(f'line {number}: {error}')
    return rows, errors

PREFIX = 'gfx_cpu_cost_'
META = {'report_version': 1, 'sample_period': 64,
        'index_primitive_known': 0, 'index_generation_measured': 0}
COUNTS = (
    'clock_calls', 'descriptor_calls', 'descriptor_output_bytes',
    'pipeline_calls', 'pipeline_hash_input_bytes', 'pipeline_hit_calls',
    'pipeline_miss_calls', 'pipeline_created_calls', 'index_calls',
    'index_requested_bytes', 'index_allocate_success_calls',
    'index_allocate_failure_calls', 'index_copy_calls', 'index_copied_bytes',
    'index_sampled_calls', 'index_sampled_requested_bytes',
)
PHASES = (
    'descriptor_sampled', 'pipeline_hit_inclusive', 'pipeline_miss_inclusive',
    'pipeline_miss_lookup_exclusive', 'pipeline_miss_shader_info_exclusive',
    'pipeline_miss_layout_exclusive', 'pipeline_miss_insert_exclusive',
    'index_allocate_success_sampled', 'index_allocate_failure_sampled',
    'index_copy_sampled',
)
FIELDS = ('calls', 'bytes', 'wall_us', 'rejected_clocks')
DATA = set(COUNTS) | {p + '_' + f for p in PHASES for f in FIELDS}
REQUIRED = DATA | set(META) | {'timers_enabled'}


def validate(v: dict) -> None:
    if any(v[k] != expected for k, expected in META.items()):
        raise ValueError('unsupported CPU probe version, period or measurement scope')
    if v['timers_enabled'] not in (0, 1):
        raise ValueError('invalid timer mode')
    if v['pipeline_hit_calls'] + v['pipeline_miss_calls'] != v['pipeline_calls']:
        raise ValueError('pipeline routes do not cover calls')
    if v['pipeline_created_calls'] > v['pipeline_miss_calls']:
        raise ValueError('created pipelines exceed misses')
    if v['index_allocate_success_calls'] + v['index_allocate_failure_calls'] != v['index_calls']:
        raise ValueError('index allocation routes do not cover calls')
    if v['index_copy_calls'] != v['index_allocate_success_calls']:
        raise ValueError('index copies differ from successful allocations')
    if not v['index_copied_bytes'] <= v['index_requested_bytes']:
        raise ValueError('copied bytes exceed requested bytes')
    if v['index_sampled_requested_bytes'] > v['index_requested_bytes']:
        raise ValueError('sampled bytes exceed requested bytes')
    for phase in PHASES:
        calls = v[phase + '_calls']
        if v[phase + '_rejected_clocks'] > calls:
            raise ValueError('rejected clocks exceed phase calls')
        if not calls and any(v[phase + '_' + f] for f in FIELDS[1:]):
            raise ValueError('phase data without calls')
    if not v['timers_enabled']:
        if any(v[k] for k in DATA if k not in set(COUNTS) - {
                'clock_calls', 'index_sampled_calls', 'index_sampled_requested_bytes'}):
            raise ValueError('timing samples present with timers disabled')
        return
    for phase, full in (('descriptor_sampled', 'descriptor_calls'),
                        ('index_sampled', 'index_calls')):
        sampled = v[phase + '_calls']
        if not v[full] // 64 <= sampled <= v[full] // 64 + 1:
            raise ValueError('sampling coverage is not one selected call per 64-call block')
    if v['pipeline_hit_inclusive_calls'] != v['pipeline_hit_calls']:
        raise ValueError('pipeline hit timings differ from full hit count')
    if v['pipeline_miss_inclusive_calls'] != v['pipeline_miss_calls']:
        raise ValueError('pipeline miss timings differ from full miss count')
    miss_phases = PHASES[3:7]
    if any(v[p + '_calls'] != v['pipeline_miss_calls'] for p in miss_phases):
        raise ValueError('incomplete pipeline miss phases')
    rejected = sum(v[p + '_rejected_clocks'] for p in miss_phases)
    if not rejected and not v['pipeline_miss_inclusive_rejected_clocks']:
        if sum(v[p + '_wall_us'] for p in miss_phases) != v['pipeline_miss_inclusive_wall_us']:
            raise ValueError('exclusive miss spans do not partition the inclusive duration')
    if (v['index_allocate_success_sampled_calls'] + v['index_allocate_failure_sampled_calls']
            != v['index_sampled_calls']):
        raise ValueError('sampled index allocation routes do not cover calls')
    if v['index_copy_sampled_calls'] != v['index_allocate_success_sampled_calls']:
        raise ValueError('sampled copies differ from sampled successful allocations')
    expected_clocks = (2 * v['descriptor_sampled_calls']
                       + 2 * v['pipeline_hit_calls'] + v['pipeline_miss_calls']
                       + sum(v[p + '_calls'] for p in miss_phases)
                       + 2 * v['index_sampled_calls'] + v['index_copy_sampled_calls'])
    if v['clock_calls'] != expected_clocks:
        raise ValueError('probe clock-call accounting mismatch')


def complete_reports(rows: list[dict]) -> tuple[list[dict], bool]:
    reports = []
    current = None
    for row in rows:
        name = row['event']
        if not name.startswith(PREFIX):
            continue
        key = name[len(PREFIX):]
        value = row.get('value')
        if type(value) is not int or not 0 <= value <= (1 << 63) - 1:
            raise ValueError('CPU probe requires nonnegative signed64 counters')
        if key == 'report_begin':
            if current is not None:
                raise ValueError('incomplete CPU report before next begin')
            if value == 0 or value % 60:
                raise ValueError('invalid CPU report frame boundary')
            current = {'frame': value, 'ms': row['ms'], 'values': {}}
            continue
        if current is None:
            raise ValueError('CPU metric outside framed report')
        if key == 'report_end':
            if value != current['frame'] or current['values'].keys() != REQUIRED:
                raise ValueError('incomplete or mismatched CPU report')
            validate(current['values'])
            if reports:
                before = reports[-1]
                if current['frame'] <= before['frame'] or current['ms'] < before['ms']:
                    raise ValueError('overlapping or regressing CPU reports')
                if current['values']['timers_enabled'] != before['values']['timers_enabled']:
                    raise ValueError('timer mode changed; isolate one configuration')
                if any(current['values'][k] < before['values'][k] for k in DATA):
                    raise ValueError('CPU counters regressed; isolate one runtime lifetime')
            reports.append(current)
            current = None
            continue
        if key not in REQUIRED or key in current['values']:
            raise ValueError('unknown or duplicate CPU metric: ' + key)
        current['values'][key] = value
    return reports, current is not None


def summarize(reports: list[dict], after_ms: int = 0) -> dict:
    if type(after_ms) is not int or after_ms < 0:
        raise ValueError('nonnegative after-ms required')
    if not reports:
        raise ValueError('no complete CPU reports')
    last = reports[-1]
    base = None
    if after_ms:
        base = next((r for r in reports if r['ms'] >= after_ms), None)
        if base is None or base is last:
            raise ValueError('need two complete reports after the requested boundary')
    v = {k: last['values'][k] - (base['values'][k] if base else 0) for k in DATA}
    phases = {}
    for p in PHASES:
        phases[p] = {f: v[p + '_' + f] for f in FIELDS}
        phases[p]['mean_valid_wall_us'] = (v[p + '_wall_us'] /
            (v[p + '_calls'] - v[p + '_rejected_clocks'])
            if v[p + '_calls'] > v[p + '_rejected_clocks'] else None)
    return {
        'status': 'CPU DIAGNOSTIC ONLY / NO PERFORMANCE ACCEPTANCE',
        'from_ms': base['ms'] if base else 0, 'to_ms': last['ms'],
        'from_frame': base['frame'] if base else 0, 'to_frame': last['frame'],
        'timers_enabled': last['values']['timers_enabled'],
        'counts': {k: v[k] for k in COUNTS}, 'phases': phases,
        'limitations': [
            'Wall times include preemption and probe overhead; they are not GPU or active CPU time.',
            'Descriptor/index times are samples; never multiply by64 into a claimed frame-time saving.',
            'Pipeline inclusive totals overlap their exclusive miss spans; never add them together.',
            'Index generation and primitive eligibility are not measured. All-copy cost is not V57 benefit.',
            'Whole-boot or after-ms differences can include loading and scene changes; not scene attribution.',
            'No rendering, gameplay, utilization, physical FPS or stable30 verdict.',
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--after-ms', type=int, default=0)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('choose a new output; evidence is not overwritten')
    rows, errors = parse_receipts(args.receipt)
    if errors or sum(r['event'] == 'boot' for r in rows) != 1:
        raise ValueError('use a valid receipt containing exactly one boot')
    reports, incomplete = complete_reports(rows)
    result = summarize(reports, args.after_ms)
    result['incomplete_trailing_report'] = incomplete
    result['input'] = {'path': str(args.receipt.resolve()), 'sha256': sha256(args.receipt)}
    write_json(args.output, result)
    print(json.dumps({k: result[k] for k in ('status', 'from_ms', 'to_ms', 'counts')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
