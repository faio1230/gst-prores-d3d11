"""GStreamer CPU/DX11経路を同一素材・I422_10LE条件で反復測定する。"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def write_summaries(rows, output_directory):
    (output_directory / 'summary.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    grouped = {}
    for row in rows:
        if row.get('returncode'):
            continue
        grouped.setdefault((Path(row['input']).name, row['mode']), []).append(row)
    keys = ['steady_fps', 'interval_p50_ms', 'interval_p95_ms', 'interval_p99_ms',
            'first_buffer_ms', 'seek_first_p95_ms', 'cpu_machine_percent',
            'peak_working_set_mib', 'private_mib_end']
    gpu_keys = ['gpu_util_mean_percent', 'gpu_util_p95_percent',
                'gpu_memory_max_mib', 'gpu_power_mean_w']
    medians = []
    for (source, mode), group in sorted(grouped.items()):
        record = {'input': source, 'mode': mode, 'repeats': len(group),
                  'gpu_completion_wait': group[0]['gpu_completion_wait']}
        for key in keys:
            record[key] = statistics.median(row[key] for row in group)
        for key in gpu_keys:
            values = [row['gpu'][key] for row in group if row['gpu'].get(key) is not None]
            record[key] = statistics.median(values) if values else None
        medians.append(record)
    (output_directory / 'medians.json').write_text(json.dumps(medians, indent=2), encoding='utf-8')


def number(value):
    try:
        return float(value.split()[0].replace('%', ''))
    except (AttributeError, ValueError, IndexError):
        return None


def gpu_summary(path):
    if not path.exists() or not path.stat().st_size:
        return {'samples': 0}
    with path.open(newline='', encoding='utf-8-sig') as stream:
        rows = list(csv.DictReader(stream))
    def column(fragment):
        key = next((key for key in rows[0] if fragment in key), None) if rows else None
        return [parsed for row in rows if key and (parsed := number(row.get(key))) is not None]
    utilization = column('utilization.gpu')
    memory = column('memory.used')
    power = column('power.draw')
    ordered = sorted(utilization)
    return {
        'samples': len(rows),
        'gpu_util_mean_percent': statistics.fmean(utilization) if utilization else None,
        'gpu_util_p95_percent': ordered[math.ceil(.95 * (len(ordered) - 1))] if ordered else None,
        'gpu_memory_max_mib': max(memory) if memory else None,
        'gpu_power_mean_w': statistics.fmean(power) if power else None,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('inputs', nargs='*', type=Path)
    parser.add_argument('--modes', nargs='+', default=['dx11-direct', 'dx11-download', 'cpu'])
    parser.add_argument('--loops', type=int, default=6)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--warmup', type=int, default=30)
    parser.add_argument('--frames-per-loop', type=int, default=180)
    parser.add_argument('--seeks', type=int, default=4)
    parser.add_argument('--startups', type=int, default=0)
    parser.add_argument('--minimum-seconds', type=int, default=0)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--gst-root', type=Path,
                        default=Path('C:/Program Files/gstreamer/1.0/msvc_x86_64'))
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/vs18')
    parser.add_argument('--out', type=Path, default=ROOT / 'results/d3d11-plugin-benchmark')
    parser.add_argument('--summarize-only', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.summarize_only:
        summary_path = args.out / 'summary.json'
        rows = json.loads(summary_path.read_text(encoding='utf-8'))
        for row in rows:
            result_path = Path(row['command'][3]).with_suffix('.json')
            gpu_path = Path(row['command'][3]).with_suffix('.gpu.csv')
            row['gpu'] = gpu_summary(gpu_path)
            result_path.write_text(json.dumps(row, indent=2), encoding='utf-8')
        write_summaries(rows, args.out)
        return
    if not args.inputs:
        parser.error('at least one input is required unless --summarize-only is used')
    executable = args.build_dir / 'Release/d3d11_plugin_bench.exe'
    plugin_directory = args.build_dir / 'plugins/Release'
    required = [executable, plugin_directory / 'gstproresd3d11.dll',
                plugin_directory / 'prores_vld.cso', plugin_directory / 'prores_idct_unorm.cso',
                args.gst_root / 'bin/gst-launch-1.0.exe', *args.inputs]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise SystemExit('missing required path: ' + ', '.join(missing))
    env = dict(os.environ)
    env['PATH'] = str(args.gst_root / 'bin') + os.pathsep + env.get('PATH', '')
    env['GST_PLUGIN_PATH'] = str(plugin_directory)
    env['GST_REGISTRY'] = str(args.build_dir / 'plugin-benchmark-registry.bin')
    env.pop('PRORES_DX11_SHADER_DIR', None)
    rows = []
    for source in args.inputs:
        for repeat in range(args.repeats):
            modes = args.modes if repeat % 2 == 0 else list(reversed(args.modes))
            for mode in modes:
                stem = args.out / f'{source.stem}-{mode}-{repeat}'
                command = [str(executable), str(source.resolve()), mode,
                           str(stem.with_suffix('.csv')), str(args.loops), str(args.warmup),
                           str(args.frames_per_loop), str(args.seeks), str(args.startups),
                           str(args.minimum_seconds)]
                monitor = None
                gpu_path = stem.with_suffix('.gpu.csv')
                with gpu_path.open('w', encoding='utf-8', newline='') as gpu, \
                     stem.with_suffix('.log').open('w', encoding='utf-8') as log:
                    try:
                        monitor = subprocess.Popen([
                            'nvidia-smi',
                            '--query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.used,power.draw',
                            '--format=csv', '-lms', '200'], stdout=gpu, stderr=subprocess.DEVNULL)
                    except FileNotFoundError:
                        pass
                    try:
                        try:
                            run = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                                 stderr=log, text=True, timeout=args.timeout)
                        except subprocess.TimeoutExpired:
                            log.write(f'\nSUPERVISOR: timeout after {args.timeout} seconds\n')
                            run = subprocess.CompletedProcess(command, 124, stdout='')
                    finally:
                        if monitor is not None:
                            monitor.terminate()
                            monitor.wait(timeout=10)
                row = {'input': str(source), 'repeat': repeat, 'command': command,
                       'returncode': run.returncode, 'gpu': gpu_summary(gpu_path)}
                if run.returncode == 0:
                    row.update(json.loads(run.stdout))
                else:
                    row.update(mode=mode, error='see .log')
                stem.with_suffix('.json').write_text(json.dumps(row, indent=2), encoding='utf-8')
                rows.append(row)
                write_summaries(rows, args.out)
                print(source.name, mode, repeat, row.get('steady_fps', 'FAILED'), flush=True)
                time.sleep(.2)
    if any(row['returncode'] for row in rows):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
