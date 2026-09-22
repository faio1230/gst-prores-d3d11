"""完了待ちを含むCPU/Vulkan比較。GPU統計はデバイス全体でありプロセス専用ではない。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1'

def main():
    p = argparse.ArgumentParser()
    p.add_argument('inputs', nargs='+', type=Path)
    p.add_argument('--loops', type=int, default=10)
    p.add_argument('--repeats', type=int, default=3)
    p.add_argument('--threads', type=int, default=0)
    p.add_argument('--layers', type=int, default=0)
    p.add_argument('--modes', nargs='+', default=['cpu', 'vulkan', 'download', 'cpu-d3d11', 'download-d3d11'])
    p.add_argument('--out', type=Path, default=ROOT / 'results/bench')
    p.add_argument('--exe', type=Path, default=ROOT / 'build/vs18/Release/prores_bench.exe')
    p.add_argument('--timeout', type=float, default=60)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PATH=str(SDK / 'bin') + os.pathsep + os.environ['PATH'])
    env['PRORES_COMPOSE_LAYERS'] = str(a.layers)
    rows = []
    for source in a.inputs:
        for rep in range(a.repeats):
            # 実行順の一方向の温度バイアスを軽減。キャッシュは消去しない。
            modes = a.modes if rep % 2 == 0 else list(reversed(a.modes))
            for mode in modes:
                stem = a.out / f'{source.stem}-{mode}-{rep}'
                cmd = [str(a.exe), str(source.resolve()), mode, str(stem.with_suffix('.csv')), str(a.loops), str(a.threads)]
                monitor = None
                with stem.with_suffix('.gpu.csv').open('w') as gpu, stem.with_suffix('.log').open('w') as log:
                    try:
                        monitor = subprocess.Popen(['nvidia-smi', '--query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.used,power.draw', '--format=csv', '-lms', '200'], stdout=gpu, stderr=subprocess.DEVNULL)
                    except FileNotFoundError:
                        pass
                    try:
                        try:
                            run = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=log, text=True, timeout=a.timeout)
                        except subprocess.TimeoutExpired:
                            log.write(f'\nSUPERVISOR: timeout after {a.timeout} seconds\n')
                            run = subprocess.CompletedProcess(cmd,124,stdout='')
                    finally:
                        if monitor is not None:
                            monitor.terminate()
                            monitor.wait(timeout=10)
                row = {'input': str(source), 'repeat': rep, 'command': cmd, 'returncode': run.returncode}
                if run.returncode == 0:
                    row.update(json.loads(run.stdout))
                else:
                    row.update(mode=mode, error='see .log')
                stem.with_suffix('.json').write_text(json.dumps(row, indent=2), encoding='utf-8')
                rows.append(row)
                (a.out / 'summary.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
                print(source.name, mode, rep, row.get('steady_fps', 'FAILED'), flush=True)
                time.sleep(.2)
    (a.out / 'summary.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
    if any(r['returncode'] for r in rows):
        raise SystemExit(1)

if __name__ == '__main__':
    main()
