"""PresentMonのOS表示記録をGStreamer表示試行のPIDと照合する。"""
import argparse
from bisect import bisect_left, bisect_right
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


def rows_for_trial(rows, record, present_csv):
    """同じPIDが再利用されてもQPCでその試行のPresentだけを選ぶ。"""
    if ('qpc_origin_ms' not in record or not rows or
            'CPUStartQPCTimeInMs' not in rows[0] or not present_csv.is_file()):
        return rows
    with present_csv.open(newline='', encoding='utf-8-sig') as stream:
        wall_times = [float(row['wall_ms']) for row in csv.DictReader(stream)]
    if not wall_times:
        return rows
    origin = float(record['qpc_origin_ms'])
    lower = origin + min(wall_times) - 2.0
    upper = origin + max(wall_times) + 2.0
    return [row for row in rows if (start := metric(row, 'CPUStartQPCTimeInMs'))
            is not None and (busy := metric(row, 'MsCPUBusy')) is not None
            and lower <= start + busy <= upper]


def load_stages(stage_csv):
    if not stage_csv.is_file():
        return {}
    times = {}
    previous = {}
    with stage_csv.open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            if not row.get('pts_ns'):
                continue
            stage, pts = row['stage'], int(row['pts_ns'])
            prior_loop, prior_pts = previous.get(stage, (0, pts))
            loop = prior_loop + (pts < prior_pts)
            previous[stage] = (loop, pts)
            times[(stage, loop, pts)] = float(row['wall_ms'])
    return times


def align_pts(rows, record, present_csv, stage_csv, display_column):
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
    present_wall = {(int(row['loop']), int(row['pts_ns'])): float(row['wall_ms'])
                    for row in signals}
    stage_times = load_stages(stage_csv)
    window_states = defaultdict(set)
    if signals[0].get('window_found') not in (None, '', '-1'):
        for row in signals:
            key = (int(row['loop']), int(row['pts_ns']))
            state = ('found={window_found} visible={window_visible} minimized={window_minimized} '
                     'foreground={window_foreground} size={window_width}x{window_height}').format(**row)
            if row.get('window_foreground_overlap_percent') not in (None, '', '-1'):
                state += f" foreground_overlap={row['window_foreground_overlap_percent']}%"
            if row.get('window_topmost') not in (None, '', '-1'):
                state += f" topmost={row['window_topmost']}"
            window_states[key].add(state)
    used = set()
    captured = set()
    displayed = set()
    matched_rows = defaultdict(list)
    matched_events = []
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
        matched_rows[key].append(row)
        matched_events.append((row, key))
        if metric(row, display_column) is not None:
            displayed.add(key)
    bounds = {}
    for loop, pts in all_frames:
        first, last = bounds.get(loop, (pts, pts))
        bounds[loop] = (min(first, pts), max(last, pts))
    interior = {(loop, pts) for loop, pts in all_frames
                if pts not in bounds[loop]}
    missing_display = sorted(captured - displayed)
    window_by_result = {'displayed': Counter(), 'not_displayed': Counter()}
    rows_by_result = {'displayed': [], 'not_displayed': []}
    stage_latencies = {'displayed': defaultdict(list), 'not_displayed': defaultdict(list)}
    for key in interior & captured:
        result = 'displayed' if key in displayed else 'not_displayed'
        states = window_states.get(key)
        window_by_result[result][' / '.join(sorted(states)) if states else 'not-recorded'] += 1
        rows_by_result[result].extend(matched_rows[key])
        loop, pts = key
        for before, after in (('compressed', 'decoded'), ('decoded', 'rgb'),
                              ('rgb', 'sink_push'), ('sink_push', 'sink_return')):
            start, end = stage_times.get((before, loop, pts)), stage_times.get((after, loop, pts))
            if start is not None and end is not None:
                stage_latencies[result][before + '_to_' + after + '_ms'].append(end - start)
        push_time = stage_times.get(('sink_push', loop, pts))
        if push_time is not None:
            stage_latencies[result]['sink_push_to_present_signal_ms'].append(
                present_wall[key] - push_time)
    display_events = sorted(
        (start + busy + delay, key)
        for row, key in matched_events
        if (start := metric(row, 'CPUStartQPCTimeInMs')) is not None
        and (busy := metric(row, 'MsCPUBusy')) is not None
        and (delay := metric(row, display_column)) is not None)
    display_times = [time for time, _ in display_events]
    event_position = {id(row): i for i, (row, _) in enumerate(matched_events)}
    headroom = []
    first_refresh_delay = []
    next_present_before_refresh = 0
    next_present_observed = 0
    missing_with_gpu_completion = 0
    next_display_position = Counter()
    cadence_examples = []
    for key in sorted(interior & captured - displayed):
        row = matched_rows[key][0]
        present_ms = metric(row, 'CPUStartQPCTimeInMs') + metric(row, 'MsCPUBusy')
        position = bisect_right(display_times, present_ms)
        if position == len(display_times):
            continue
        refresh_ms, refresh_key = display_events[position]
        if refresh_key[0] != key[0]:
            next_display_position['different_loop'] += 1
        elif refresh_key[1] < key[1]:
            next_display_position['earlier_pts'] += 1
        elif refresh_key[1] > key[1]:
            next_display_position['later_pts'] += 1
        else:
            next_display_position['same_pts'] += 1
        first_refresh_delay.append(refresh_ms - present_ms)
        gpu_delay = metric(row, 'MsRenderPresentLatency')
        gpu_headroom = None
        if gpu_delay is not None:
            missing_with_gpu_completion += 1
            gpu_headroom = refresh_ms - (present_ms + gpu_delay)
            headroom.append(gpu_headroom)
        next_present_ms = None
        for later, later_key in matched_events[event_position[id(row)] + 1:]:
            if later_key != key:
                next_present_ms = metric(later, 'CPUStartQPCTimeInMs') + metric(later, 'MsCPUBusy')
                break
        if next_present_ms is not None:
            next_present_observed += 1
            if next_present_ms <= refresh_ms:
                next_present_before_refresh += 1
        if len(cadence_examples) < 12:
            cadence_examples.append({'loop': key[0], 'pts_ns': key[1],
                                     'gpu_headroom_to_next_display_ms': gpu_headroom,
                                     'next_present_before_next_display':
                                     next_present_ms <= refresh_ms if next_present_ms is not None else None})
    return {'method': 'QPC + MsCPUBusy とpresent通知を1ms以内で照合',
            'matched_presentmon_rows': len(used),
            'unmatched_presentmon_rows': len(rows) - len(used),
            'max_match_delta_ms': max(deltas, default=None),
            'source_present_frames': len(all_frames),
            'captured_source_frames': len(captured),
            'confirmed_displayed_source_frames': len(displayed),
            'captured_but_not_displayed_source_frames': len(missing_display),
            'uncaptured_source_frames': len(all_frames - captured),
            'uncaptured_first_32': [{'loop': loop, 'pts_ns': pts}
                                    for loop, pts in sorted(all_frames - captured)[:32]],
            'duplicate_presentmon_pts_first_32': [
                {'loop': loop, 'pts_ns': pts, 'rows': len(matched_rows[(loop, pts)])}
                for loop, pts in sorted(matched_rows)
                if len(matched_rows[(loop, pts)]) > 1][:32],
            'interior_source_frames': len(interior),
            'interior_confirmed_displayed': len(interior & displayed),
            'interior_captured_but_not_displayed': len(interior & captured - displayed),
            'interior_uncaptured': len(interior - captured),
            'interior_window_state_by_display': {key: dict(value)
                                                 for key, value in window_by_result.items()},
            'interior_presentmon_metrics_by_display': {
                key: {'between_presents_ms': percentiles(value, 'MsBetweenPresents'),
                      'present_api_ms': percentiles(value, 'MsInPresentAPI'),
                      'gpu_busy_ms': percentiles(value, 'MsGPUBusy'),
                      'render_present_latency_ms': percentiles(value, 'MsRenderPresentLatency')}
                for key, value in rows_by_result.items()},
            'interior_stage_latency_by_display': {
                result: {name: percentiles([{'value': item} for item in values], 'value')
                         for name, values in metrics.items()}
                for result, metrics in stage_latencies.items()},
            'undisplayed_cadence': {
                'samples_with_next_display': len(first_refresh_delay),
                'samples_with_gpu_completion': missing_with_gpu_completion,
                'gpu_headroom_to_next_display_ms': percentiles(
                    [{'value': value} for value in headroom], 'value'),
                'gpu_ready_at_least_2ms_before_next_display': sum(value >= 2 for value in headroom),
                'gpu_ready_at_least_5ms_before_next_display': sum(value >= 5 for value in headroom),
                'gpu_completed_after_next_display': sum(value < 0 for value in headroom),
                'next_display_source_position': dict(next_display_position),
                'next_present_observed': next_present_observed,
                'next_present_before_next_display': next_present_before_refresh,
                'first_next_display_delay_ms': percentiles(
                    [{'value': value} for value in first_refresh_delay], 'value'),
                'examples': cadence_examples},
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
        present_csv = path.with_suffix('.present.csv')
        rows = rows_for_trial(by_pid[str(pid)], record, present_csv)
        visible = [row for row in rows if metric(row, display_column) is not None]
        present_count = record['present_count']
        # PresentMonは開始・終了端で1件程度少なく、sink自身の追加Presentで多くなる場合もある。
        coverage_sufficient = present_count > 0 and len(rows) >= present_count - 2
        modes = Counter(row.get('PresentMode', '') for row in rows)
        push = record.get('stages', {}).get('stage_latency', {}).get(
            'sink_push_to_sink_return_ms', {})
        trials.append({'input': record['input'], 'repeat': record['repeat'],
                       'process_id': pid, 'gstreamer_rendered': record['rendered'],
                       'sink_ts_offset_ms': record.get('sink_ts_offset_ms', 0),
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
                       'pts_alignment': align_pts(rows, record, present_csv,
                                                  path.with_suffix('.stages.csv'),
                                                  display_column),
                       'max_sink_push_ms': push.get('max')})
    trials.sort(key=lambda trial: trial['repeat'])
    if len({trial['input'] for trial in trials}) != 1:
        parser.error('複数素材は--stemで分ける')
    if len({trial['sink_ts_offset_ms'] for trial in trials}) != 1:
        parser.error('同期時刻の異なる試行は別々に集計する')
    covered = [trial for trial in trials if trial['presentmon_coverage_sufficient']]
    aligned = [trial for trial in covered if trial['pts_alignment']]
    summary = {'source': trials[0]['input'], 'presentmon_csv': str(args.presentmon_csv),
               'sink_ts_offset_ms': trials[0]['sink_ts_offset_ms'],
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
