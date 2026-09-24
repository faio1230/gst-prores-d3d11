"""実素材の全フレームを固定FFmpeg CPUとDX11で比較する。中間rawは終了時に削除する。"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
GST = Path('C:/Program Files/gstreamer/1.0/msvc_x86_64/bin')
PLUGIN = ROOT / 'build/vs18/plugins/Release'
RGB_PROBE = ROOT / 'build/vs18/Release/d3d11_rgb_probe.exe'
RGB_SHADER = PLUGIN / 'prores_rgb.cso'
RGB_COLORIMETRY = '1:1:5:1'  # full-range RGB, BT.709 transfer and primaries


def execute(command, log, env=None):
    with log.open('w', encoding='utf-8') as output:
        result = subprocess.run([str(item) for item in command], stdout=output,
                                stderr=subprocess.STDOUT, env=env, timeout=900,
                                check=False)
    if result.returncode:
        raise RuntimeError(f'exit {result.returncode}: {command[0]} (log: {log})')


def gst_command(source, destination, mode, frames):
    cmd = [GST / 'gst-launch-1.0.exe', '-q', '-e', 'filesrc',
           f'location={source.as_posix()}', '!', 'qtdemux', '!', 'proresd3d11dec', '!']
    if mode in ('rgb_gpu', 'rgb_element'):
        cmd += ['d3d11convert' if mode == 'rgb_gpu' else 'proresd3d11rgb', '!',
                f'video/x-raw(memory:D3D11Memory),format=RGB10A2_LE,colorimetry={RGB_COLORIMETRY}', '!']
    cmd += ['d3d11download', '!']
    if mode == 'rgb_cpu':
        # d3d11convert samples 4:2:2 chroma at centered positions.  Without
        # chroma-site the CPU path defaults to left-cosited interpolation.
        cmd += ['video/x-raw,format=I422_10LE,colorimetry=bt709,chroma-site=jpeg', '!',
                'videoconvert', 'dither=none', 'chroma-resampler=linear',
                'matrix-mode=full', 'gamma-mode=none', 'primaries-mode=none', '!',
                f'video/x-raw,format=RGB10A2_LE,colorimetry={RGB_COLORIMETRY}', '!']
    elif mode in ('rgb_gpu', 'rgb_element'):
        cmd += [f'video/x-raw,format=RGB10A2_LE,colorimetry={RGB_COLORIMETRY}', '!']
    if frames:
        cmd += ['identity', f'eos-after={frames + 1}', '!']
    return cmd + ['filesink', f'location={destination.as_posix()}']


def make_stat(name):
    return dict(channel=name, max_abs=0, absolute_sum=0, signed_sum=0, different_samples=0,
                samples=0, squared_sum=0, frames_with_difference=0)


def update(stat, left, right, count_frame=True):
    difference = left.astype(np.int32) - right.astype(np.int32)
    absolute = np.abs(difference)
    frame_max = int(absolute.max())
    stat['max_abs'] = max(stat['max_abs'], frame_max)
    stat['absolute_sum'] += int(absolute.sum(dtype=np.int64))
    stat['signed_sum'] += int(difference.sum(dtype=np.int64))
    stat['different_samples'] += int(np.count_nonzero(difference))
    stat['samples'] += difference.size
    stat['squared_sum'] += int(np.square(difference.astype(np.int64)).sum(dtype=np.int64))
    if count_frame:
        stat['frames_with_difference'] += int(frame_max != 0)
    return frame_max


def compare(left_path, right_path, width, height, mode, expected_frames):
    pixel_count = width * height
    if mode == 'yuv':
        frame_bytes = pixel_count * 4
        sections = [('Y', pixel_count), ('U', pixel_count // 2),
                    ('V', pixel_count // 2)]
    else:
        frame_bytes = pixel_count * 4
        sections = [('R', 0), ('G', 10), ('B', 20), ('A', 30)]
    lengths = [path.stat().st_size for path in (left_path, right_path)]
    if lengths[0] != lengths[1] or lengths[0] % frame_bytes:
        raise ValueError(f'raw size mismatch: {lengths}, expected frame bytes {frame_bytes}')
    frames = lengths[0] // frame_bytes
    if frames != expected_frames:
        raise ValueError(f'frame count mismatch: actual {frames}, expected {expected_frames}')
    stats = [make_stat(name) for name, _ in sections]
    frame_maxima = []
    with left_path.open('rb') as left, right_path.open('rb') as right:
        for _ in range(frames):
            left_bytes = left.read(frame_bytes)
            right_bytes = right.read(frame_bytes)
            if mode == 'yuv':
                aa = np.frombuffer(left_bytes, dtype='<u2')
                bb = np.frombuffer(right_bytes, dtype='<u2')
                offset = 0
                maxima = []
                for stat, (_, count) in zip(stats, sections):
                    maxima.append(update(stat, aa[offset:offset + count],
                                         bb[offset:offset + count]))
                    offset += count
            else:
                aa = np.frombuffer(left_bytes, dtype='<u4')
                bb = np.frombuffer(right_bytes, dtype='<u4')
                maxima = []
                for stat, (_, shift) in zip(stats, sections):
                    mask = 3 if shift == 30 else 1023
                    maxima.append(update(stat, (aa >> shift) & mask,
                                         (bb >> shift) & mask))
            frame_maxima.append(maxima)
    for stat in stats:
        stat['mae'] = stat['absolute_sum'] / stat['samples']
        stat['bias'] = stat['signed_sum'] / stat['samples']
        mse = stat['squared_sum'] / stat['samples']
        stat['psnr_db'] = (10 * math.log10((3 if stat['channel'] == 'A' else 1023) ** 2 / mse)
                           if mse else None)
    return dict(frames=frames, channels=stats, per_frame_maxima=frame_maxima)


def compare_formula(yuv_path, rgb_path, width, height, expected_frames):
    """実際のDX11 I422 codeに独立BT.709式を適用しGPU RGBと照合する。"""
    pixels = width * height
    frame_bytes = pixels * 4
    if (yuv_path.stat().st_size != expected_frames * frame_bytes or
            rgb_path.stat().st_size != expected_frames * frame_bytes):
        raise ValueError('BT.709数式比較のrawサイズ・フレーム数が一致しない')
    stats = [make_stat(name) for name in 'RGBA']
    frame_maxima = []
    x = np.arange(width, dtype=np.float32)
    location = (x - .5) * .5
    base = np.floor(location).astype(np.int32)
    low = np.clip(base, 0, width // 2 - 1)
    high = np.clip(base + 1, 0, width // 2 - 1)
    fraction = (location - np.floor(location)).astype(np.float32)
    with yuv_path.open('rb') as yuv_file, rgb_path.open('rb') as rgb_file:
        for _ in range(expected_frames):
            raw = np.frombuffer(yuv_file.read(frame_bytes), dtype='<u2')
            packed = np.frombuffer(rgb_file.read(frame_bytes), dtype='<u4').reshape(height, width)
            y = raw[:pixels].reshape(height, width)
            u = raw[pixels:pixels + pixels // 2].reshape(height, width // 2)
            v = raw[pixels + pixels // 2:].reshape(height, width // 2)
            maxima = [0, 0, 0, 0]
            for row in range(0, height, 64):
                rows = slice(row, min(row + 64, height))
                yy = (y[rows].astype(np.float32) - 64.0) / 876.0
                cb = (u[rows][:, low].astype(np.float32) * (1.0 - fraction) +
                      u[rows][:, high].astype(np.float32) * fraction - 512.0) / 896.0
                cr = (v[rows][:, low].astype(np.float32) * (1.0 - fraction) +
                      v[rows][:, high].astype(np.float32) * fraction - 512.0) / 896.0
                expected = (yy + 1.5748 * cr,
                            yy - 0.187324 * cb - 0.468124 * cr,
                            yy + 1.8556 * cb)
                for index, (stat, predicted) in enumerate(zip(stats, expected)):
                    reference = np.rint(np.clip(predicted * 1023.0, 0, 1023)).astype(np.int32)
                    actual = (packed[rows] >> (index * 10)) & 1023
                    maxima[index] = max(maxima[index],
                                        update(stat, reference, actual, count_frame=False))
                maxima[3] = max(maxima[3], update(stats[3],
                    np.full(packed[rows].shape, 3, dtype=np.int32),
                    (packed[rows] >> 30) & 3, count_frame=False))
            for stat, maximum in zip(stats, maxima):
                stat['frames_with_difference'] += int(maximum != 0)
            frame_maxima.append(maxima)
    for stat in stats:
        stat['mae'] = stat['absolute_sum'] / stat['samples']
        stat['bias'] = stat['signed_sum'] / stat['samples']
        mse = stat['squared_sum'] / stat['samples']
        stat['psnr_db'] = (10 * math.log10((3 if stat['channel'] == 'A' else 1023) ** 2 / mse)
                           if mse else None)
    return dict(frames=expected_frames, channels=stats, per_frame_maxima=frame_maxima)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--mode', choices=('yuv', 'rgb', 'rgb_native', 'rgb_formula',
                                           'rgb_element', 'rgb_element_formula'), required=True)
    parser.add_argument('--frames', type=int, help='先頭Nフレーム。省略時は全フレーム')
    parser.add_argument('--diagnostic-dir', type=Path,
                        help='RGB先頭1フレームのI422・CPU RGB・GPU RGB rawを保存')
    parser.add_argument('--scratch-dir', type=Path,
                        help='大きな中間raw用の一時領域。省略時はbuild/')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.diagnostic_dir and (args.mode not in ('rgb', 'rgb_native', 'rgb_element')
                                or args.frames != 1):
        parser.error('--diagnostic-dir は --mode rgb/rgb_native/rgb_element --frames 1 と併用する')
    source = args.input.resolve()
    probe = json.loads(subprocess.check_output([
        str(SDK / 'ffprobe.exe'), '-v', 'error', '-select_streams', 'v:0',
        '-show_streams', '-of', 'json', str(source)]))['streams'][0]
    if (probe['codec_name'] != 'prores' or probe['pix_fmt'] != 'yuv422p10le'
            or probe.get('profile', '').lower() not in ('proxy', 'lt', 'standard', 'hq')):
        raise ValueError(f'not supported ProRes 422 profile: {probe}')
    expected_frames = min(args.frames or int(probe['nb_frames']), int(probe['nb_frames']))
    width, height = probe['width'], probe['height']
    if width % 2:
        raise ValueError('odd width is not supported by this raw comparator')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    (ROOT / 'build').mkdir(exist_ok=True)
    scratch = (args.scratch_dir or ROOT / 'build').resolve()
    if not scratch.is_dir():
        parser.error(f'一時領域が存在しない: {scratch}')
    raw_bytes = width * height * 4 * expected_frames * 2
    free_bytes = shutil.disk_usage(scratch).free
    if free_bytes < raw_bytes + 1024 ** 3:
        parser.error(f'一時領域が不足: 必要約{(raw_bytes + 1024 ** 3) / 1024 ** 3:.1f} GiB、'
                     f'空き{free_bytes / 1024 ** 3:.1f} GiB ({scratch})')
    env = dict(os.environ)
    env['PATH'] = str(GST) + os.pathsep + env.get('PATH', '')
    env['GST_PLUGIN_PATH'] = str(PLUGIN)
    env['GST_REGISTRY'] = str(ROOT / 'build/vs18/plugin-real-quality-registry.bin')
    env.pop('PRORES_DX11_SHADER_DIR', None)
    runs = []
    with tempfile.TemporaryDirectory(prefix='real-quality-', dir=scratch) as directory:
        temp = Path(directory)
        left, right = temp / 'reference.raw', temp / 'dx11.raw'
        if args.mode == 'yuv':
            # Compare the coded full raster: some MOVs carry clean-aperture
            # cropping metadata that FFmpeg otherwise applies automatically.
            command = [SDK / 'ffmpeg.exe', '-v', 'error', '-xerror', '-threads', '1',
                       '-apply_cropping', '0',
                       '-i', source, '-map', '0:v:0', '-frames:v', str(expected_frames),
                       '-fps_mode', 'passthrough', '-c:v', 'rawvideo',
                       '-pix_fmt', 'yuv422p10le', '-f', 'rawvideo', left]
            execute(command, args.out.with_suffix('.cpu.log'))
            runs.append([str(item) for item in command])
            command = gst_command(source, right, 'yuv', args.frames)
            execute(command, args.out.with_suffix('.dx11.log'), env)
            runs.append([str(item) for item in command])
        else:
            command = gst_command(source, left,
                                  'yuv' if args.mode in ('rgb_formula', 'rgb_element_formula')
                                  else 'rgb_cpu', args.frames)
            execute(command, args.out.with_suffix('.cpu.log'), env)
            runs.append([str(item) for item in command])
            command = ([RGB_PROBE, source, right, RGB_SHADER,
                        str(expected_frames if args.frames else 0)]
                       if args.mode in ('rgb_native', 'rgb_formula')
                       else gst_command(source, right,
                                        'rgb_element' if args.mode in ('rgb_element',
                                                                       'rgb_element_formula')
                                        else 'rgb_gpu', args.frames))
            execute(command, args.out.with_suffix('.dx11.log'), env)
            runs.append([str(item) for item in command])
            if args.diagnostic_dir:
                yuv = temp / 'dx11-i422.raw'
                command = gst_command(source, yuv, 'yuv', 1)
                execute(command, args.out.with_suffix('.yuv.log'), env)
                runs.append([str(item) for item in command])
                args.diagnostic_dir.mkdir(parents=True, exist_ok=True)
                for name, path in (('dx11-i422.raw', yuv), ('cpu-rgb10a2.raw', left),
                                   ('gpu-rgb10a2.raw', right)):
                    shutil.copyfile(path, args.diagnostic_dir / name)
        result = (compare_formula(left, right, width, height, expected_frames)
                  if args.mode in ('rgb_formula', 'rgb_element_formula')
                  else compare(left, right, width, height,
                               'yuv' if args.mode == 'yuv' else 'rgb', expected_frames))
    with source.open('rb') as stream:
        source_sha256 = hashlib.file_digest(stream, 'sha256').hexdigest()
    result.update(input=str(source), source_sha256=source_sha256,
                  mode=args.mode, reference=('fixed FFmpeg 8.1 CPU yuv422p10le' if args.mode == 'yuv'
                                            else 'same DX11 I422 + independent BT.709 limited-to-full centered chroma formula'
                                            if args.mode in ('rgb_formula', 'rgb_element_formula')
                                            else 'D3D11 decoded I422 + CPU videoconvert BT.709, centered 4:2:2 chroma'),
                  probe=probe, commands=runs)
    result['passed'] = (all(channel['max_abs'] <= 1 for channel in result['channels'])
                        if args.mode in ('yuv', 'rgb_formula', 'rgb_element_formula') else None)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'frames': result['frames'], 'channels': result['channels']},
                     ensure_ascii=False, indent=2))
    if result['passed'] is False:
        raise SystemExit('全フレーム比較で最大許容差1を超過')


if __name__ == '__main__':
    main()
