"""複数の純粋DX11 GStreamerデコーダーを同じGPUで同時実行する。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=Path)
    parser.add_argument('--count', type=int, default=2)
    parser.add_argument('--loops', type=int, default=6)
    parser.add_argument('--warmup', type=int, default=30)
    parser.add_argument('--frames-per-loop', type=int, default=180)
    parser.add_argument('--mode', choices=['dx11-direct', 'dx11-download'],
                        default='dx11-download')
    parser.add_argument('--gst-root', type=Path,
                        default=Path('C:/Program Files/gstreamer/1.0/msvc_x86_64'))
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/vs18')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.count <= 8:
        parser.error('count must be 1..8')
    args.out.mkdir(parents=True, exist_ok=True)
    executable = args.build_dir / 'Release/d3d11_plugin_bench.exe'
    plugin_directory = args.build_dir / 'plugins/Release'
    required = [args.input, executable, plugin_directory / 'gstproresd3d11.dll',
                plugin_directory / 'prores_vld.cso', plugin_directory / 'prores_idct_unorm.cso']
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise SystemExit('missing required path: ' + ', '.join(missing))
    base_env = dict(os.environ)
    base_env['PATH'] = str(args.gst_root / 'bin') + os.pathsep + base_env.get('PATH', '')
    base_env['GST_PLUGIN_PATH'] = str(plugin_directory)
    base_env.pop('PRORES_DX11_SHADER_DIR', None)
    processes = []
    gpu_log = (args.out / 'gpu.csv').open('w', encoding='utf-8')
    monitor = None
    try:
        monitor = subprocess.Popen([
            'nvidia-smi',
            '--query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.used,power.draw',
            '--format=csv', '-lms', '200'], stdout=gpu_log, stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        pass
    begin = time.perf_counter()
    try:
        for index in range(args.count):
            log = (args.out / f'{index}.log').open('w', encoding='utf-8')
            command = [str(executable), str(args.input.resolve()), args.mode,
                       str(args.out / f'{index}.csv'), str(args.loops), str(args.warmup),
                       str(args.frames_per_loop), '0', '0']
            env = dict(base_env)
            env['GST_REGISTRY'] = str(args.build_dir / f'plugin-stress-registry-{index}.bin')
            process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                       stderr=log, text=True)
            processes.append((process, log, command))
        rows = []
        for process, log, command in processes:
            stdout, _ = process.communicate(timeout=300)
            log.close()
            if process.returncode:
                raise RuntimeError('stress process failed; see log')
            row = json.loads(stdout)
            row['command'] = command
            rows.append(row)
    finally:
        for process, log, _ in processes:
            if process.poll() is None:
                process.kill()
                process.wait()
            if not log.closed:
                log.close()
        if monitor is not None:
            monitor.terminate()
            monitor.wait(timeout=10)
        gpu_log.close()
    result = {
        'passed': True,
        'input': str(args.input),
        'mode': args.mode,
        'count': args.count,
        'device_scope': 'independent GstD3D11Device per process on the same adapter',
        'wall_seconds': time.perf_counter() - begin,
        'aggregate_steady_fps': sum(row['steady_fps'] for row in rows),
        'processes': rows,
    }
    (args.out / 'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
