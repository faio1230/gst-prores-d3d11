"""復号器を通らないD3D11表示対照のPresentMon CSVをPID別に集計する。"""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def stats(rows, column):
    values = sorted(value for row in rows if (value := number(row.get(column))) is not None)
    if not values:
        return {'samples': 0, 'p50': None, 'p95': None, 'p99': None, 'max': None}
    return {'samples': len(values),
            'p50': values[math.ceil(.50 * (len(values) - 1))],
            'p95': values[math.ceil(.95 * (len(values) - 1))],
            'p99': values[math.ceil(.99 * (len(values) - 1))],
            'max': values[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('presentmon_csv', type=Path)
    parser.add_argument('--expected-frames', type=int, required=True)
    parser.add_argument('--expected-trials', type=int)
    parser.add_argument('--boundary-frames', type=int, default=3)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if (args.expected_frames < 1 or args.boundary_frames < 0 or
            (args.expected_trials is not None and args.expected_trials < 1)):
        parser.error('expected-frames/trialsは正、boundary-framesは非負を指定する')
    by_pid = defaultdict(list)
    with args.presentmon_csv.open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            by_pid[row['ProcessID']].append(row)
    if not by_pid:
        parser.error('PresentMonに対象プロセスの行がない')
    if args.expected_trials is not None and len(by_pid) != args.expected_trials:
        parser.error(f'試行数が一致しない: PID {len(by_pid)}件、予定{args.expected_trials}件。'
                     'PID再利用または捕捉漏れを確認する')
    trials = []
    for pid, rows in by_pid.items():
        rows.sort(key=lambda row: float(row['CPUStartQPCTimeInMs']))
        border = min(args.boundary_frames, len(rows) // 2)
        interior = rows[border:len(rows) - border]
        displayed = [row for row in interior
                     if number(row.get('MsUntilDisplayed')) is not None]
        not_displayed = [row for row in interior
                         if number(row.get('MsUntilDisplayed')) is None]
        trials.append({
            'pid': int(pid), 'presentmon_rows': len(rows),
            'expected_source_frames': args.expected_frames,
            'interior_rows': len(interior),
            'interior_without_display_time': len(not_displayed),
            'present_modes': dict(Counter(row['PresentMode'] for row in rows)),
            'between_presents_ms': stats(interior, 'MsBetweenPresents'),
            'between_display_change_ms': stats(interior, 'MsBetweenDisplayChange'),
            'render_present_latency_ms': stats(interior, 'MsRenderPresentLatency'),
            'until_displayed_ms': stats(interior, 'MsUntilDisplayed'),
            'present_api_ms': stats(interior, 'MsInPresentAPI'),
            'gpu_busy_ms': stats(interior, 'MsGPUBusy'),
            'timing_by_display': {
                label: {'between_presents_ms': stats(group, 'MsBetweenPresents'),
                        'render_present_latency_ms': stats(group, 'MsRenderPresentLatency'),
                        'gpu_busy_ms': stats(group, 'MsGPUBusy')}
                for label, group in (('displayed', displayed),
                                     ('not_displayed', not_displayed))}})
    trials.sort(key=lambda item: item['pid'])
    result = {'presentmon_csv': str(args.presentmon_csv),
              'boundary_frames_each_end': args.boundary_frames,
              'caveat': 'PTS照合・sink rendered/drop統計はない。PID別の捕捉済みPresent行だけを集計。',
              'trials': trials,
              'total_interior_rows': sum(t['interior_rows'] for t in trials),
              'total_interior_without_display_time': sum(
                  t['interior_without_display_time'] for t in trials)}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'trials': len(trials),
                      'total_interior_rows': result['total_interior_rows'],
                      'total_interior_without_display_time':
                      result['total_interior_without_display_time']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
