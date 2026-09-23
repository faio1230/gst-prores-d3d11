"""復号器なしD3D11 4K60表示対照を、ProRes表示ベンチと同じPTS/sink統計で測る。"""
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GST = Path('C:/Program Files/gstreamer/1.0/msvc_x86_64/bin')
PLUGIN = ROOT / 'build/vs18/plugins/Release'
BENCH = ROOT / 'build/vs18/Release/d3d11_display_bench.exe'
GPU_STATS = runpy.run_path(str(ROOT / 'scripts/benchmark-d3d11-display.py'))['gpu_stats']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('rgb', 'heavy'), required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--topmost-window', action='store_true')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error('--repeatsは正の数を指定する')
    mode = 'testsrc-' + args.mode
    args.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = str(GST) + os.pathsep + env.get('PATH', '')
    env['GST_PLUGIN_PATH'] = str(PLUGIN)
    env['GST_REGISTRY'] = str(ROOT / 'build/vs18/plugin-reference-display-registry.bin')
    env.pop('PRORES_DX11_SHADER_DIR', None)
    for repeat in range(args.repeats):
        stem = args.out / f'{mode}-{repeat}'
        monitor_path = stem.with_suffix('.gpu.csv')
        command = [str(BENCH), mode, '1', str(stem.with_suffix('.present.csv')),
                   str(stem.with_suffix('.stages.csv')), 'trace-sink-return',
                   'trace-window-state']
        if args.topmost_window:
            command.append('topmost-window')
        monitor = None
        with monitor_path.open('w', encoding='utf-8', newline='') as gpu:
            try:
                monitor = subprocess.Popen([
                    'nvidia-smi', '--query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.used,power.draw',
                    '--format=csv', '-lms', '200'], stdout=gpu, stderr=subprocess.DEVNULL)
            except FileNotFoundError:
                pass
            try:
                with stem.with_suffix('.stderr.log').open('w', encoding='utf-8') as errors:
                    process = subprocess.run(command, env=env, text=True,
                                             stdout=subprocess.PIPE, stderr=errors,
                                             timeout=120, check=False)
            finally:
                if monitor is not None:
                    monitor.terminate()
                    monitor.wait(timeout=10)
        if process.returncode:
            raise RuntimeError(f'{mode} repeat {repeat} exit {process.returncode}; '
                               f'see {stem.with_suffix(".stderr.log")}')
        record = json.loads(process.stdout)
        if record['source_mode'] != mode:
            raise ValueError(f'unexpected source mode: {record["source_mode"]}')
        record.update(input=mode, repeat=repeat, source_frames=1440,
                      gpu=GPU_STATS(monitor_path), command=command)
        stem.with_suffix('.json').write_text(json.dumps(record, ensure_ascii=False, indent=2),
                                             encoding='utf-8')
        print(json.dumps({'mode': mode, 'repeat': repeat,
                          'rendered': record['rendered'], 'dropped': record['dropped'],
                          'present_count': record['present_count'],
                          'p95_ms': record['interval_p95_ms'],
                          'p99_ms': record['interval_p99_ms']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
