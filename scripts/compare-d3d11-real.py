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


def gst_command(source, destination, mode, frames, pixel_format='yuv422p10le'):
    cmd = [GST / 'gst-launch-1.0.exe', '-q', '-e', 'filesrc',
           f'location={source.as_posix()}', '!', 'qtdemux', '!', 'proresd3d11dec', '!']
    if mode in ('rgb_gpu', 'rgb_element'):
        cmd += ['d3d11convert' if mode == 'rgb_gpu' else 'proresd3d11rgb', '!',
                f'video/x-raw(memory:D3D11Memory),format=RGB10A2_LE,colorimetry={RGB_COLORIMETRY}', '!']
    cmd += ['d3d11download', '!']
    if mode == 'yuv':
        gst_format = {'yuv422p10le': 'I422_10LE', 'yuv444p10le': 'Y444_10LE',
                      'yuv422p12le': 'I422_12LE', 'yuv444p12le': 'Y444_12LE'}[pixel_format]
        cmd += [f'video/x-raw,format={gst_format}', '!']
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
    if destination is None:
        return cmd + ['fdsink', 'fd=1']
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


def compare(left_path, right_path, width, height, mode, expected_frames, pixel_format='yuv422p10le'):
    pixel_count = width * height
    if mode == 'yuv':
        chroma_samples = pixel_count if '444' in pixel_format else pixel_count // 2
        frame_bytes = (pixel_count + 2 * chroma_samples) * 2
        sections = [('Y', pixel_count), ('U', chroma_samples), ('V', chroma_samples)]
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
        peak = 3 if stat['channel'] == 'A' else 4095 if mode == 'yuv' and 'p12le' in pixel_format else 1023
        stat['psnr_db'] = (10 * math.log10(peak ** 2 / mse)
                           if mse else None)
    return dict(frames=frames, channels=stats, per_frame_maxima=frame_maxima)


def compare_yuv_streams(cpu_command, dx_command, width, height, frames,
                        pixel_format, cpu_log, dx_log, env, timeout=900):
    """一時RAWを作らず2つの独立した復号器を全フレーム・全画素比較する。"""
    pixels = width * height
    chroma_width = width if '444' in pixel_format else width // 2
    chroma_samples = chroma_width * height
    sections = [('Y', pixels), ('U', chroma_samples), ('V', chroma_samples)]
    frame_bytes = (pixels + 2 * chroma_samples) * 2
    plane_widths = (width, chroma_width, chroma_width)
    # GstVideoInfo's planar 16-bit rows are aligned to four bytes.  The
    # fixed-SDK rawvideo stream has no row padding, so strip it only on DX11.
    plane_strides = tuple((plane_width + 1) & ~1 for plane_width in plane_widths)
    dx_frame_bytes = sum(stride * height * 2 for stride in plane_strides)
    stats = [make_stat(name) for name, _ in sections]
    maxima = []
    with cpu_log.open('wb') as cpu_error, dx_log.open('wb') as dx_error:
        cpu = subprocess.Popen([str(item) for item in cpu_command], stdout=subprocess.PIPE,
                               stderr=cpu_error, stdin=subprocess.DEVNULL, env=env)
        dx = subprocess.Popen([str(item) for item in dx_command], stdout=subprocess.PIPE,
                              stderr=dx_error, stdin=subprocess.DEVNULL, env=env)
        try:
            for index in range(frames):
                left = cpu.stdout.read(frame_bytes)
                right = dx.stdout.read(dx_frame_bytes)
                if len(left) != frame_bytes or len(right) != dx_frame_bytes:
                    raise RuntimeError(f'frame {index}: CPU={len(left)}, DX11={len(right)}, '
                                       f'expected={frame_bytes}/{dx_frame_bytes}')
                aa = np.frombuffer(left, dtype='<u2')
                padded = np.frombuffer(right, dtype='<u2')
                offset = 0
                dx_offset = 0
                frame_maxima = []
                for stat, (_, count), plane_width, stride in zip(
                        stats, sections, plane_widths, plane_strides):
                    plane_size = stride * height
                    bb = padded[dx_offset:dx_offset + plane_size].reshape(
                        height, stride)[:, :plane_width].reshape(-1)
                    frame_maxima.append(update(stat, aa[offset:offset + count],
                                               bb))
                    offset += count
                    dx_offset += plane_size
                maxima.append(frame_maxima)
            if cpu.stdout.read(1) or dx.stdout.read(1):
                raise RuntimeError('expected frame count exceeded')
            if cpu.wait(timeout=timeout) or dx.wait(timeout=timeout):
                raise RuntimeError(f'CPU exit={cpu.returncode}, DX11 exit={dx.returncode}; '
                                   f'logs: {cpu_log}, {dx_log}')
        finally:
            for process in (cpu, dx):
                if process.poll() is None:
                    process.kill()
                process.wait()
                process.stdout.close()
    peak = 4095 if 'p12le' in pixel_format else 1023
    for stat in stats:
        stat['mae'] = stat['absolute_sum'] / stat['samples']
        stat['bias'] = stat['signed_sum'] / stat['samples']
        mse = stat['squared_sum'] / stat['samples']
        stat['psnr_db'] = 10 * math.log10(peak ** 2 / mse) if mse else None
    return dict(frames=frames, channels=stats, per_frame_maxima=maxima)


def compare_rgb_formula_streams(yuv_command, rgb_command, width, height, frames,
                                pixel_format, yuv_log, rgb_log, env, timeout=900):
    """同一DX11復号のYUVとGPU RGBを、独立BT.709式で全フレーム比較する。"""
    pixels = width * height
    chroma_width = width if '444' in pixel_format else width // 2
    chroma_samples = chroma_width * height
    plane_widths = (width, chroma_width, chroma_width)
    plane_strides = tuple((plane_width + 1) & ~1 for plane_width in plane_widths)
    yuv_bytes = sum(stride * height * 2 for stride in plane_strides)
    rgb_bytes = pixels * 4
    stats = [make_stat(name) for name in 'RGBA']
    maxima = []
    scale = 4.0 if 'p12le' in pixel_format else 1.0
    if '422' in pixel_format:
        x = np.arange(width, dtype=np.float32)
        location = (x - .5) * .5
        base = np.floor(location).astype(np.int32)
        low = np.clip(base, 0, width // 2 - 1)
        high = np.clip(base + 1, 0, width // 2 - 1)
        fraction = (location - np.floor(location)).astype(np.float32)
    with yuv_log.open('wb') as yuv_error, rgb_log.open('wb') as rgb_error:
        yuv = subprocess.Popen([str(item) for item in yuv_command], stdout=subprocess.PIPE,
                               stderr=yuv_error, stdin=subprocess.DEVNULL, env=env)
        rgb = subprocess.Popen([str(item) for item in rgb_command], stdout=subprocess.PIPE,
                               stderr=rgb_error, stdin=subprocess.DEVNULL, env=env)
        try:
            for index in range(frames):
                yuv_raw = yuv.stdout.read(yuv_bytes)
                rgb_raw = rgb.stdout.read(rgb_bytes)
                if len(yuv_raw) != yuv_bytes or len(rgb_raw) != rgb_bytes:
                    raise RuntimeError(f'frame {index}: YUV={len(yuv_raw)}, RGB={len(rgb_raw)}, '
                                       f'expected={yuv_bytes}/{rgb_bytes}')
                raw = np.frombuffer(yuv_raw, dtype='<u2')
                packed = np.frombuffer(rgb_raw, dtype='<u4').reshape(height, width)
                planes = []
                offset = 0
                for plane_width, stride in zip(plane_widths, plane_strides):
                    planes.append(raw[offset:offset + stride * height]
                                  .reshape(height, stride)[:, :plane_width])
                    offset += stride * height
                y_plane, u_plane, v_plane = planes
                frame_maxima = [0, 0, 0, 0]
                for row in range(0, height, 64):
                    rows = slice(row, min(row + 64, height))
                    yy = (y_plane[rows].astype(np.float32) / scale - 64.0) / 876.0
                    if '444' in pixel_format:
                        cb_codes = u_plane[rows].astype(np.float32)
                        cr_codes = v_plane[rows].astype(np.float32)
                    else:
                        cb_codes = (u_plane[rows][:, low].astype(np.float32) * (1.0 - fraction) +
                                    u_plane[rows][:, high].astype(np.float32) * fraction)
                        cr_codes = (v_plane[rows][:, low].astype(np.float32) * (1.0 - fraction) +
                                    v_plane[rows][:, high].astype(np.float32) * fraction)
                    cb = (cb_codes / scale - 512.0) / 896.0
                    cr = (cr_codes / scale - 512.0) / 896.0
                    expected = (yy + 1.5748 * cr,
                                yy - 0.187324 * cb - 0.468124 * cr,
                                yy + 1.8556 * cb)
                    for channel, (stat, predicted) in enumerate(zip(stats, expected)):
                        reference = np.rint(np.clip(predicted * 1023.0, 0, 1023)).astype(np.int32)
                        actual = (packed[rows] >> (channel * 10)) & 1023
                        frame_maxima[channel] = max(frame_maxima[channel],
                                                    update(stat, reference, actual, False))
                    frame_maxima[3] = max(frame_maxima[3], update(stats[3],
                        np.full(packed[rows].shape, 3, dtype=np.int32),
                        (packed[rows] >> 30) & 3, False))
                for stat, maximum in zip(stats, frame_maxima):
                    stat['frames_with_difference'] += int(maximum != 0)
                maxima.append(frame_maxima)
            if yuv.stdout.read(1) or rgb.stdout.read(1):
                raise RuntimeError('expected RGB frame count exceeded')
            if yuv.wait(timeout=timeout) or rgb.wait(timeout=timeout):
                raise RuntimeError(f'YUV exit={yuv.returncode}, RGB exit={rgb.returncode}; '
                                   f'logs: {yuv_log}, {rgb_log}')
        finally:
            for process in (yuv, rgb):
                if process.poll() is None:
                    process.kill()
                process.wait()
                process.stdout.close()
    for stat in stats:
        stat['mae'] = stat['absolute_sum'] / stat['samples']
        stat['bias'] = stat['signed_sum'] / stat['samples']
        mse = stat['squared_sum'] / stat['samples']
        peak = 3 if stat['channel'] == 'A' else 1023
        stat['psnr_db'] = 10 * math.log10(peak ** 2 / mse) if mse else None
    return dict(frames=frames, channels=stats, per_frame_maxima=maxima)


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
    parser.add_argument('--streaming', action='store_true',
                        help='一時RAWなしでCPU/DX11出力をフレーム単位で比較（YUVのみ）')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.diagnostic_dir and (args.mode not in ('rgb', 'rgb_native', 'rgb_element')
                                or args.frames != 1):
        parser.error('--diagnostic-dir は --mode rgb/rgb_native/rgb_element --frames 1 と併用する')
    if args.streaming and args.mode not in ('yuv', 'rgb_element_formula'):
        parser.error('--streaming は --mode yuv/rgb_element_formula のみ対応')
    source = args.input.resolve()
    probe = json.loads(subprocess.check_output([
        str(SDK / 'ffprobe.exe'), '-v', 'error', '-select_streams', 'v:0',
        '-show_streams', '-of', 'json', str(source)]))['streams'][0]
    pixel_format = probe['pix_fmt']
    if (probe['codec_name'] != 'prores' or
            pixel_format not in ('yuv422p10le', 'yuv444p10le', 'yuv422p12le', 'yuv444p12le') or
            probe.get('codec_tag_string') not in ('apco', 'apcs', 'apcn', 'apch', 'ap4h', 'ap4x')):
        raise ValueError(f'not supported alpha-free ProRes format: {probe}')
    if args.mode not in ('yuv', 'rgb_element_formula') and pixel_format != 'yuv422p10le':
        raise ValueError('RGB比較は現在I422_10LEのみ対応')
    expected_frames = min(args.frames or int(probe['nb_frames']), int(probe['nb_frames']))
    width, height = probe['width'], probe['height']
    if width % 2 and (not args.streaming or
                      args.mode not in ('yuv', 'rgb_element_formula') or
                      '422' in pixel_format):
        raise ValueError('odd width is not supported by this raw comparator')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    (ROOT / 'build').mkdir(exist_ok=True)
    scratch = (args.scratch_dir or ROOT / 'build').resolve()
    if not scratch.is_dir():
        parser.error(f'一時領域が存在しない: {scratch}')
    raw_frame_bytes = width * height * (6 if '444' in pixel_format else 4)
    raw_bytes = raw_frame_bytes * expected_frames * 2
    free_bytes = shutil.disk_usage(scratch).free
    if not args.streaming and free_bytes < raw_bytes + 1024 ** 3:
        parser.error(f'一時領域が不足: 必要約{(raw_bytes + 1024 ** 3) / 1024 ** 3:.1f} GiB、'
                     f'空き{free_bytes / 1024 ** 3:.1f} GiB ({scratch})')
    env = dict(os.environ)
    env['PATH'] = str(GST) + os.pathsep + env.get('PATH', '')
    env['GST_PLUGIN_PATH'] = str(PLUGIN)
    env['GST_REGISTRY'] = str(ROOT / 'build/vs18/plugin-real-quality-registry.bin')
    env.pop('PRORES_DX11_SHADER_DIR', None)
    runs = []
    if args.streaming:
        if args.mode == 'yuv':
            cpu_command = [SDK / 'ffmpeg.exe', '-v', 'error', '-xerror', '-threads', '1',
                           '-apply_cropping', '0', '-i', source, '-map', '0:v:0',
                           '-frames:v', str(expected_frames), '-fps_mode', 'passthrough',
                           '-c:v', 'rawvideo', '-pix_fmt', pixel_format, '-f', 'rawvideo', '-']
            dx_command = gst_command(source, None, 'yuv', args.frames, pixel_format)
            result = compare_yuv_streams(cpu_command, dx_command, width, height,
                                         expected_frames, pixel_format,
                                         args.out.with_suffix('.cpu.log'),
                                         args.out.with_suffix('.dx11.log'), env)
            runs.extend(([str(item) for item in cpu_command],
                         [str(item) for item in dx_command]))
        else:
            yuv_command = gst_command(source, None, 'yuv', args.frames, pixel_format)
            rgb_command = gst_command(source, None, 'rgb_element', args.frames, pixel_format)
            result = compare_rgb_formula_streams(yuv_command, rgb_command, width, height,
                                                 expected_frames, pixel_format,
                                                 args.out.with_suffix('.yuv.log'),
                                                 args.out.with_suffix('.rgb.log'), env)
            runs.extend(([str(item) for item in yuv_command],
                         [str(item) for item in rgb_command]))
    else:
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
                       '-pix_fmt', pixel_format, '-f', 'rawvideo', left]
            execute(command, args.out.with_suffix('.cpu.log'))
            runs.append([str(item) for item in command])
            command = gst_command(source, right, 'yuv', args.frames, pixel_format)
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
                               'yuv' if args.mode == 'yuv' else 'rgb', expected_frames,
                               pixel_format))
    with source.open('rb') as stream:
        source_sha256 = hashlib.file_digest(stream, 'sha256').hexdigest()
    result.update(input=str(source), source_sha256=source_sha256,
                  mode=args.mode, reference=(f'fixed FFmpeg 8.1 CPU {pixel_format}' if args.mode == 'yuv'
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
