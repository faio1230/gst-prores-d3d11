"""D3D11実表示経路を反復し、present/QoS/CPU/GPUを個別に保存する。"""
import argparse
from collections import Counter
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GST = Path('C:/Program Files/gstreamer/1.0/msvc_x86_64/bin')
SDK = ROOT / 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
BENCH = ROOT / 'build/vs18/Release/d3d11_display_bench.exe'
PLUGIN = ROOT / 'build/vs18/plugins/Release'
DIAGNOSTIC_D3D11_DLL = GST.parent / 'lib/gstreamer-1.0/gstd3d11.dll'
DIAGNOSTIC_D3D11_SHA256 = 'b6156f2299ab0af570b7935138b1389b5f91c84b9a6e0ed755cd15b2e5aa4992'


def gpu_stats(path):
    with path.open(newline='', encoding='utf-8-sig') as stream:
        rows = list(csv.DictReader(stream))
    result = {'samples': len(rows)}
    for fragment, name in [('utilization.gpu', 'gpu_util_percent'),
                           ('memory.used', 'gpu_memory_mib'),
                           ('power.draw', 'gpu_power_w')]:
        column = next((key for key in rows[0] if fragment in key), None) if rows else None
        values = []
        for row in rows:
            try:
                values.append(float(row[column].split()[0]))
            except (KeyError, AttributeError, ValueError, TypeError):
                pass
        result[name + '_mean'] = statistics.fmean(values) if values else None
        result[name + '_p95'] = (sorted(values)[math.ceil(.95 * (len(values) - 1))]
                                       if values else None)
        result[name + '_max'] = max(values) if values else None
    return result


def stage_stats(path, expected_frames, loops, rendered, postrgb_queue=False):
    events = {}
    counts = Counter()
    stage_names = ('demuxed', 'compressed', 'decoded', 'rgb', 'rgb_dequeued',
                   'sink_push', 'sink_return')
    wall_by_stage = {name: {} for name in stage_names}
    widest_gaps = {}
    with path.open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            name = row['stage']
            pts = int(row['pts_ns'])
            wall = float(row['wall_ms'])
            previous = events.get(name)
            loop = previous['loop'] + int(pts < previous['pts']) if previous else 0
            if previous and loop == previous['loop']:
                gap = wall - previous['wall']
                if gap > widest_gaps.get(name, {}).get('interval_ms', 0):
                    widest_gaps[name] = {'interval_ms': gap, 'loop': loop,
                                         'before_pts_ns': previous['pts'], 'after_pts_ns': pts}
            events[name] = {'pts': pts, 'wall': wall, 'loop': loop}
            counts[(name, loop, pts)] += 1
            wall_by_stage[name][(loop, pts)] = wall
    by_stage = {name: {(loop, pts) for stage, loop, pts in counts if stage == name}
                for name in stage_names}
    missing_before_decoder = sorted(by_stage['demuxed'] - by_stage['compressed'])
    missing_after_decoder = sorted(by_stage['compressed'] - by_stage['decoded'])
    missing_after_converter = sorted(by_stage['decoded'] - by_stage['rgb'])
    missing_in_postrgb_queue = (sorted(by_stage['rgb'] - by_stage['rgb_dequeued'])
                                if postrgb_queue else [])
    latencies = {}
    pairs = [('compressed', 'decoded'), ('decoded', 'rgb')]
    if by_stage['demuxed']:
        pairs.insert(0, ('demuxed', 'compressed'))
    if postrgb_queue:
        pairs.append(('rgb', 'rgb_dequeued'))
    if by_stage['sink_push']:
        pairs.append(('rgb_dequeued' if postrgb_queue else 'rgb', 'sink_push'))
        pairs.append(('sink_push', 'sink_return'))
    for before, after in pairs:
        durations = sorted(wall_by_stage[after][key] - wall_by_stage[before][key]
                           for key in by_stage[before] & by_stage[after])
        latencies[f'{before}_to_{after}_ms'] = {
            'p95': durations[math.ceil(.95 * (len(durations) - 1))] if durations else None,
            'p99': durations[math.ceil(.99 * (len(durations) - 1))] if durations else None,
            'max': durations[-1] if durations else None}
    expected = expected_frames * loops
    return {'expected_frames': expected,
            'demuxed': len(by_stage['demuxed']) if by_stage['demuxed'] else None,
            'compressed': len(by_stage['compressed']),
            'decoded': len(by_stage['decoded']),
            'rgb': len(by_stage['rgb']),
            'rgb_dequeued': len(by_stage['rgb_dequeued']) if postrgb_queue else None,
            'sink_push': len(by_stage['sink_push']) if by_stage['sink_push'] else None,
            'sink_return': len(by_stage['sink_return']) if by_stage['sink_return'] else None,
            'rendered': rendered,
            'end_to_end_missing': expected - rendered,
            'missing_before_decoder': missing_before_decoder[:32],
            'missing_before_decoder_count': len(missing_before_decoder),
            'missing_after_decoder': missing_after_decoder[:32],
            'missing_after_decoder_count': len(missing_after_decoder),
            'missing_after_converter': missing_after_converter[:32],
            'missing_after_converter_count': len(missing_after_converter),
            'missing_in_postrgb_queue': missing_in_postrgb_queue[:32],
            'missing_in_postrgb_queue_count': (len(missing_in_postrgb_queue)
                                               if postrgb_queue else None),
            'widest_same_loop_gap': widest_gaps,
            'stage_latency': latencies}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--loops', type=int, default=3)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--preroll', action='store_true', help='先にPAUSED prerollを完了してからPLAYING')
    parser.add_argument('--lossless-sink-policy', action='store_true',
                        help='診断用: sink QoSを止めmax-lateness=-1にする')
    parser.add_argument('--native-rgb', action='store_true',
                        help='標準d3d11convertの代わりに専用DX11 RGB要素を表示する')
    parser.add_argument('--queue-before-decoder', action='store_true',
                        help='qtdemuxとdecoderを32 bufferのqueueで分離し前後のPTS到達時刻を記録する')
    parser.add_argument('--queue-after-rgb', action='store_true',
                        help='RGBとsinkの間を4 bufferのqueueで分離しPTS到達時刻を記録する')
    parser.add_argument('--decoder-no-qos', action='store_true',
                        help='診断用: decoderだけQoSによる遅延フレーム破棄を無効化する')
    parser.add_argument('--sink-stall-ms', type=int, default=0,
                        help='診断用: PTS 1秒のsink入力を一度だけN ms止める（最大1000）')
    parser.add_argument('--trace-sink-return', action='store_true',
                        help='診断用: RGB出力の下流pushが戻るまでをPTSごとに記録する')
    parser.add_argument('--trace-window-state', action='store_true',
                        help='診断用: present通知時に自プロセスのD3D11ウィンドウ状態を読む')
    parser.add_argument('--topmost-window', action='store_true',
                        help='診断用: 検査ウィンドウだけを一時的に最前面へ置く')
    parser.add_argument('--sink-ts-offset-ms', type=int, default=0,
                        help='診断用: sink同期時刻の相対移動（負値は早い提出、単位ms）')
    parser.add_argument('--sink-processing-deadline-ms', type=int, default=15,
                        help='診断用: sinkの処理期限。既定15msとの比較に用いる')
    parser.add_argument('--present-sync-interval', type=int, choices=(0, 1), default=0,
                        help='診断用: 固定GStreamer 1.28.2 sinkのPresent1同期値を1にする')
    parser.add_argument('--sink-no-clock-sync', action='store_true',
                        help='診断用: sinkのPTS時計同期を外し、Presentの同期だけで供給する')
    parser.add_argument('--settle-ms', type=int, default=0,
                        help='診断用: 最終EOS後、swap chain破棄前に待機（最大5000ms）')
    parser.add_argument('--out', type=Path, default=ROOT / 'results/d3d11-display')
    args = parser.parse_args()
    if args.loops < 1 or args.repeats < 1:
        parser.error('loops/repeats must be positive')
    if not 0 <= args.sink_stall_ms <= 1000:
        parser.error('--sink-stall-ms は0～1000を指定する')
    if not -100 <= args.sink_ts_offset_ms <= 100:
        parser.error('--sink-ts-offset-ms は-100～100を指定する')
    if not 0 <= args.sink_processing_deadline_ms <= 100:
        parser.error('--sink-processing-deadline-ms は0～100を指定する')
    if args.present_sync_interval == 1 and not args.preroll:
        parser.error('--present-sync-interval 1 には--prerollが必要')
    if not 0 <= args.settle_ms <= 5000:
        parser.error('--settle-ms は0～5000を指定する')
    if args.present_sync_interval == 1:
        if (not DIAGNOSTIC_D3D11_DLL.is_file() or
                hashlib.sha256(DIAGNOSTIC_D3D11_DLL.read_bytes()).hexdigest() !=
                DIAGNOSTIC_D3D11_SHA256):
            parser.error('非公開ABI診断は検証済みgstd3d11.dllのSHA256一致が必要')
    args.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = str(GST) + os.pathsep + env.get('PATH', '')
    env['GST_PLUGIN_PATH'] = str(PLUGIN)
    env['GST_REGISTRY'] = str(ROOT / 'build/vs18/plugin-display-registry.bin')
    env.pop('PRORES_DX11_SHADER_DIR', None)
    for source in args.inputs:
        source = source.resolve()
        probe = json.loads(subprocess.check_output([
            str(SDK / 'ffprobe.exe'), '-v', 'error', '-select_streams', 'v:0',
            '-show_streams', '-of', 'json', str(source)]))['streams'][0]
        for repeat in range(args.repeats):
            stem = args.out / f'{source.stem}-{repeat}'
            monitor_path = stem.with_suffix('.gpu.csv')
            monitor = None
            with monitor_path.open('w', encoding='utf-8', newline='') as gpu:
                try:
                    monitor = subprocess.Popen([
                        'nvidia-smi', '--query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.used,power.draw',
                        '--format=csv', '-lms', '200'], stdout=gpu, stderr=subprocess.DEVNULL)
                except FileNotFoundError:
                    pass
                try:
                    command = [str(BENCH), str(source), str(args.loops),
                               str(stem.with_suffix('.present.csv')),
                               str(stem.with_suffix('.stages.csv'))]
                    if args.preroll:
                        command.append('preroll')
                    if args.lossless_sink_policy:
                        command.append('lossless')
                    if args.native_rgb:
                        command.append('native-rgb')
                    if args.queue_before_decoder:
                        command.append('queue-before-decoder')
                    if args.queue_after_rgb:
                        command.append('queue-after-rgb')
                    if args.decoder_no_qos:
                        command.append('decoder-no-qos')
                    if args.sink_stall_ms:
                        command.append(f'sink-stall-ms={args.sink_stall_ms}')
                    if args.trace_sink_return:
                        command.append('trace-sink-return')
                    if args.trace_window_state:
                        command.append('trace-window-state')
                    if args.topmost_window:
                        command.append('topmost-window')
                    if args.sink_ts_offset_ms:
                        command.append(f'sink-ts-offset-ms={args.sink_ts_offset_ms}')
                    command.append(f'sink-processing-deadline-ms={args.sink_processing_deadline_ms}')
                    if args.present_sync_interval == 1:
                        command.append('present-sync1')
                    if args.sink_no_clock_sync:
                        command.append('sink-no-clock-sync')
                    if args.settle_ms:
                        command.append(f'settle-ms={args.settle_ms}')
                    with stem.with_suffix('.stderr.log').open('w', encoding='utf-8') as errors:
                        process = subprocess.run(command, env=env, text=True,
                                                 stdout=subprocess.PIPE, stderr=errors,
                                                 timeout=120, check=False)
                finally:
                    if monitor is not None:
                        monitor.terminate()
                        monitor.wait(timeout=10)
            if process.returncode:
                raise RuntimeError(f'{source.name} repeat {repeat} exit {process.returncode}; '
                                   f'see {stem.with_suffix(".stderr.log")}')
            record = json.loads(process.stdout)
            record.update(input=str(source), repeat=repeat, source_fps=probe['r_frame_rate'],
                          source_frames=int(probe['nb_frames']), gpu=gpu_stats(monitor_path),
                          stages=stage_stats(stem.with_suffix('.stages.csv'),
                                             int(probe['nb_frames']), args.loops,
                                             record['rendered'], args.queue_after_rgb),
                          command=command)
            stem.with_suffix('.json').write_text(json.dumps(record, ensure_ascii=False, indent=2),
                                                 encoding='utf-8')
            print(json.dumps({'source': source.name, 'repeat': repeat,
                              'rendered': record['rendered'], 'dropped': record['dropped'],
                              'end_to_end_missing': record['stages']['end_to_end_missing'],
                              'p95_ms': record['interval_p95_ms'],
                              'p99_ms': record['interval_p99_ms']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
