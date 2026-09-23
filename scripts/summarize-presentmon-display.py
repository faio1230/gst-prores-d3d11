"""PresentMonのOS表示記録をGStreamer表示試行のPIDと照合する。"""
import argparse
from bisect import bisect_left
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import re
import statistics


def metric(row, name):
    value = row.get(name, '')
    return None if value in ('', 'NA') else float(value)


def percentiles(rows, name):
    values = sorted(value for row in rows if (value := metric(row, name)) is not None)
    if not values:
        return {'samples': 0, 'p50': None, 'p95': None, 'p99': None, 'max': None}
    at = lambda fraction: values[math.ceil(fraction * (len(values) - 1))]
    return {'samples': len(values), 'p50': at(.5), 'p95': at(.95),
            'p99': at(.99), 'max': values[-1]}


def align_pts(rows, record, present_csv, display_column):
    if ('qpc_origin_ms' not in record or not rows or
            'CPUStartQPCTimeInMs' not in rows[0] or not present_csv.is_file()):
        return None
    with present_csv.open(newline='', encoding='utf-8-sig') as stream:
        signals = [row for row in csv.DictReader(stream) if row.get('pts_ns')]
    if not signals:
        return None
    origin = float(record['qpc_origin_ms'])
    signal_times = [origin + float(row['wall_ms']) for row in signals]
    all_frames = {(int(row['loop']), int(row['pts_ns'])) for row in signals}
    used = set()
    captured = set()
    displayed = set()
    deltas = []
    for row in rows:
        start = metric(row, 'CPUStartQPCTimeInMs')
        busy = metric(row, 'MsCPUBusy')
        if start is None or busy is None:
            continue
        target = start + busy
        position = bisect_left(signal_times, target)
        candidates = (i for i in range(max(0, position - 2),
                                       min(len(signals), position + 2)) if i not in used)
        nearest = min(candidates, key=lambda i: abs(signal_times[i] - target), default=None)
        if nearest is None or abs(signal_times[nearest] - target) > 1.0:
            continue
        used.add(nearest)
        deltas.append(abs(signal_times[nearest] - target))
        key = (int(signals[nearest]['loop']), int(signals[nearest]['pts_ns']))
        captured.add(key)
        if metric(row, display_column) is not None:
            displayed.add(key)
    bounds = {}
    for loop, pts in all_frames:
        first, last = bounds.get(loop, (pts, pts))
        bounds[loop] = (min(first, pts), max(last, pts))
    interior = {(loop, pts) for loop, pts in all_frames
                if pts not in bounds[loop]}
    missing_display = sorted(captured - displayed)
    return {'method': 'QPC + MsCPUBusy とpresent通知を1ms以内で照合',
            'matched_presentmon_rows': len(used),
            'unmatched_presentmon_rows': len(rows) - len(used),
            'max_match_delta_ms': max(deltas, default=None),
            'source_present_frames': len(all_frames),
            'captured_source_frames': len(captured),
            'confirmed_displayed_source_frames': len(displayed),
            'captured_but_not_displayed_source_frames': len(missing_display),
            'uncaptured_source_frames': len(all_frames - captured),
            'interior_source_frames': len(interior),
            'interior_confirmed_displayed': len(interior & displayed),
            'interior_captured_but_not_displayed': len(interior & captured - displayed),
            'interior_uncaptured': len(interior - captured),
            'not_displayed_first_32': missing_display[:32]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('presentmon_csv', type=Path)
    parser.add_argument('bench_directory', type=Path)
    parser.add_argument('--stem', help='素材stemで試行JSONを絞る')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    with args.presentmon_csv.open(newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or 'ProcessID' not in reader.fieldnames:
            parser.error('PresentMon CSVにProcessID列がない')
        display_column = ('MsUntilDisplayed' if 'MsUntilDisplayed' in reader.fieldnames
                          else 'DisplayedTime' if 'DisplayedTime' in reader.fieldnames else None)
        if not display_column:
            parser.error('PresentMon CSVに画面表示の判定列がない')
        by_pid = defaultdict(list)
        for row in reader:
            by_pid[row['ProcessID']].append(row)
    paths = sorted(path for path in args.bench_directory.glob('*.json')
                   if re.search(r'-\d+\.json$', path.name)
                   and (not args.stem or path.name.startswith(args.stem + '-')))
    if not paths:
        parser.error('表示試行JSONが見つからない')
    trials = []
    for path in paths:
        record = json.loads(path.read_text(encoding='utf-8'))
        pid = record.get('process_id')
        if pid is None:
            parser.error(f'{path}にprocess_idがない。計測器を更新して再測定する')
        rows = by_pid[str(pid)]
        visible = [row for row in rows if metric(row, display_column) is not None]
        present_count = record['present_count']
        # PresentMonは開始・終了端で1件程度少なく、sink自身の追加Presentで多くなる場合もある。
        coverage_sufficient = present_count > 0 and len(rows) >= present_count - 2
        modes = Counter(row.get('PresentMode', '') for row in rows)
        push = record.get('stages', {}).get('stage_latency', {}).get(
            'sink_push_to_sink_return_ms', {})
        trials.append({'input': record['input'], 'repeat': record['repeat'],
                       'process_id': pid, 'gstreamer_rendered': record['rendered'],
                       'gstreamer_dropped': record['dropped'],
                       'gstreamer_qos': record['qos_messages'],
                       'gstreamer_present_signals': present_count,
                       'presentmon_rows': len(rows),
                       'presentmon_rows_with_display_time': len(visible),
                       'presentmon_rows_without_display_time': len(rows) - len(visible),
                       'presentmon_coverage_sufficient': coverage_sufficient,
                       'present_mode_counts': dict(modes),
                       'display_change_ms': percentiles(rows, 'MsBetweenDisplayChange'),
                       'present_api_ms': percentiles(rows, 'MsInPresentAPI'),
                       'until_displayed_ms': percentiles(rows, display_column),
                       'pts_alignment': align_pts(rows, record,
                                                  path.with_suffix('.present.csv'),
                                                  display_column),
                       'max_sink_push_ms': push.get('max')})
    trials.sort(key=lambda trial: trial['repeat'])
    if len({trial['input'] for trial in trials}) != 1:
        parser.error('複数素材は--stemで分ける')
    covered = [trial for trial in trials if trial['presentmon_coverage_sufficient']]
    aligned = [trial for trial in covered if trial['pts_alignment']]
    summary = {'source': trials[0]['input'], 'presentmon_csv': str(args.presentmon_csv),
               'trials': len(trials), 'coverage_sufficient_trials': len(covered),
               'gstreamer_rendered_covered': sum(t['gstreamer_rendered'] for t in covered),
               'presentmon_rows_covered': sum(t['presentmon_rows'] for t in covered),
               'presentmon_rows_with_display_time_covered': sum(
                   t['presentmon_rows_with_display_time'] for t in covered),
               'presentmon_rows_without_display_time_covered': sum(
                   t['presentmon_rows_without_display_time'] for t in covered),
               'median_display_p95_ms': statistics.median(
                   t['display_change_ms']['p95'] for t in covered) if covered else None,
               'median_display_p99_ms': statistics.median(
                   t['display_change_ms']['p99'] for t in covered) if covered else None,
               'max_display_gap_ms': max((t['display_change_ms']['max'] for t in covered),
                                         default=None),
               'max_present_api_ms': max((t['present_api_ms']['max'] for t in covered),
                                         default=None),
               'pts_aligned_trials': len(aligned),
               'interior_source_frames_aligned': sum(
                   t['pts_alignment']['interior_source_frames'] for t in aligned),
               'interior_confirmed_displayed_aligned': sum(
                   t['pts_alignment']['interior_confirmed_displayed'] for t in aligned),
               'interior_captured_but_not_displayed_aligned': sum(
                   t['pts_alignment']['interior_captured_but_not_displayed'] for t in aligned),
               'interior_uncaptured_aligned': sum(
                   t['pts_alignment']['interior_uncaptured'] for t in aligned),
               'trial_details': trials,
               'caveat': ('PTS照合がない試行では、sinkの追加Presentやseek境界を含むため、'
                          '画面表示時刻のない行数を欠落した動画フレーム数とは同一視しない。'
                          'PTS照合ありでも最初・最後のフレームはEOS/seekの影響を受ける。'
                          'coverage_sufficientは件数による目安で、完全捕捉の証明ではない。')}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: value for key, value in summary.items()
                      if key not in ('trial_details', 'caveat')}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
