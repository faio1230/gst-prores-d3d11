"""実表示試行のsink統計と3段階のPTSログを突き合わせる。"""
import argparse
import csv
import json
from pathlib import Path
import re
import runpy
import statistics

ROOT = Path(__file__).resolve().parents[1]
STAGE_STATS = runpy.run_path(str(ROOT / 'scripts/benchmark-d3d11-display.py'))['stage_stats']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--stem', help='複数素材を同じ出力先で測った場合の素材stem')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(path for path in args.directory.glob('*.json')
                   if re.search(r'-\d+\.json$', path.name)
                   and (not args.stem or path.name.startswith(args.stem + '-')))
    if not paths:
        parser.error('試行JSONが見つからない')
    trials = []
    for path in paths:
        record = json.loads(path.read_text(encoding='utf-8'))
        stages_path = path.with_suffix('.stages.csv')
        if not stages_path.is_file():
            raise FileNotFoundError(stages_path)
        stages = STAGE_STATS(stages_path, record['source_frames'], record['loops'],
                             record['rendered'])
        with path.with_suffix('.present.csv').open(newline='', encoding='utf-8-sig') as stream:
            presents = list(csv.DictReader(stream))
        max_present_gap = max(float(row['interval_ms']) for row in presents)
        trials.append({'input': record['input'], 'repeat': record['repeat'],
                       'rgb_converter': record.get('rgb_converter', 'd3d11convert'),
                       'predecode_queue': record.get('predecode_queue', False),
                       'decoder_no_qos': record.get('decoder_no_qos', False),
                       'injected_sink_stall_ms': record.get('injected_sink_stall_ms', 0),
                       'expected': stages['expected_frames'], 'rendered': record['rendered'],
                       'sink_dropped': record['dropped'], 'qos': record['qos_messages'],
                       'end_to_end_missing': stages['end_to_end_missing'],
                       'lossless_sink_policy': record.get('lossless_sink_policy', False),
                       'preroll_ms': record.get('preroll_ms', 0),
                       'present_p95_ms': record['interval_p95_ms'],
                       'present_p99_ms': record['interval_p99_ms'],
                       'first_present_ms': float(presents[0]['wall_ms']),
                       'max_present_gap_ms': max_present_gap,
                       'seek_first_p95_ms': record['seek_first_p95_ms'],
                       'cpu_machine_percent': record['cpu_machine_percent'],
                       'gpu_util_percent_mean': record['gpu']['gpu_util_percent_mean'],
                       'peak_working_set_mib': record['peak_working_set_mib'],
                       'stages': stages})
    if len({trial['input'] for trial in trials}) != 1:
        parser.error('複数素材は別々に集計する')
    for field in ('rgb_converter', 'predecode_queue', 'decoder_no_qos',
                  'injected_sink_stall_ms',
                  'lossless_sink_policy'):
        if len({trial[field] for trial in trials}) != 1:
            parser.error(f'異なる{field}条件は別々に集計する')
    trials.sort(key=lambda trial: trial['repeat'])
    def median(name):
        values = [trial[name] for trial in trials if trial[name] is not None]
        return statistics.median(values) if values else None
    summary = {'input': trials[0]['input'],
               'rgb_converter': trials[0]['rgb_converter'],
               'predecode_queue': trials[0]['predecode_queue'],
               'decoder_no_qos': trials[0]['decoder_no_qos'],
               'injected_sink_stall_ms': trials[0]['injected_sink_stall_ms'],
               'lossless_sink_policy': trials[0]['lossless_sink_policy'],
               'repeats': len(trials),
               'total_expected': sum(trial['expected'] for trial in trials),
               'total_rendered': sum(trial['rendered'] for trial in trials),
               'total_sink_dropped': sum(trial['sink_dropped'] for trial in trials),
               'total_end_to_end_missing': sum(trial['end_to_end_missing'] for trial in trials),
               'trials_with_missing': sum(trial['end_to_end_missing'] != 0 for trial in trials),
               'median_present_p95_ms': median('present_p95_ms'),
               'median_present_p99_ms': median('present_p99_ms'),
               'median_seek_first_p95_ms': median('seek_first_p95_ms'),
               'median_cpu_machine_percent': median('cpu_machine_percent'),
               'median_gpu_util_percent_mean': median('gpu_util_percent_mean'),
               'median_peak_working_set_mib': median('peak_working_set_mib'),
               'max_present_gap_ms': max(trial['max_present_gap_ms'] for trial in trials),
               'trials': trials}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: value for key, value in summary.items() if key != 'trials'},
                     ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
