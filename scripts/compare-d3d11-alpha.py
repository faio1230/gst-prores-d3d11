"""アルファ付きProResの全フレーム・全成分を固定SDK CPUとDX11で比較する。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
FFMPEG = ROOT / 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
GST = Path('C:/Program Files/gstreamer/1.0/msvc_x86_64/bin')
PLUGIN = ROOT / 'build/vs18/plugins/Release'


def run(command, log, env=None):
    with log.open('w', encoding='utf-8') as output:
        result = subprocess.run([str(arg) for arg in command], stdout=output,
                                stderr=subprocess.STDOUT, env=env,
                                timeout=900, check=False)
    if result.returncode:
        raise RuntimeError(f'exit {result.returncode}: {command[0]} ({log})')


def probe(path):
    result = subprocess.run([str(FFMPEG / 'ffprobe.exe'), '-v', 'error',
                             '-select_streams', 'v:0', '-show_entries',
                             'stream=codec_tag_string,pix_fmt,width,height,nb_frames',
                             '-of', 'json', str(path)], capture_output=True,
                            text=True, check=True)
    return json.loads(result.stdout)['streams'][0]


def read_exact(stream, size):
    chunks = []
    received = 0
    while received < size:
        chunk = stream.read(size - received)
        if not chunk:
            raise RuntimeError(f'rawフレームが途中で終了: {received}/{size} bytes')
        chunks.append(chunk)
        received += len(chunk)
    return b''.join(chunks)


def compare(path, out, temp, rgb, streaming):
    info = probe(path)
    pixel_format = info['pix_fmt']
    if pixel_format not in {'yuva422p10le', 'yuva422p12le',
                            'yuva444p10le', 'yuva444p12le'}:
        raise ValueError(f'アルファ付きProResではない: {pixel_format}')
    width, height = int(info['width']), int(info['height'])
    # d3d11downloadはテクスチャ幅を16画素単位に丸めた行strideを維持する。
    download_stride_pixels = (width + 15) // 16 * 16
    chroma_width = width // 2 if '422' in pixel_format else width
    y_samples = width * height
    chroma_samples = chroma_width * height
    frame_samples = y_samples * 2 + chroma_samples * 2
    dx_frame_samples = download_stride_pixels * height * 4
    name = path.stem
    env = os.environ.copy()
    env['PATH'] = str(GST) + os.pathsep + env.get('PATH', '')
    env['GST_PLUGIN_PATH'] = str(PLUGIN)
    env['GST_REGISTRY'] = str(out / 'gst-registry.bin')
    cpu_command = [FFMPEG / 'ffmpeg.exe', '-hide_banner', '-loglevel', 'error',
                   '-i', path, '-map', '0:v:0', '-pix_fmt', pixel_format,
                   '-f', 'rawvideo']
    dx_command = [GST / 'gst-launch-1.0.exe', '-e', '-q',
                  'filesrc', f'location={path.as_posix()}', '!', 'qtdemux', '!',
                  'proresd3d11dec', '!', 'd3d11download', '!']

    def compare_arrays(cpu_words, dx_words, count):
        cpu_words = cpu_words.reshape(count, frame_samples)
        dx_words = dx_words.reshape(count, height, download_stride_pixels, 4)[:, :, :width, :]
        y_plane = cpu_words[:, :y_samples].reshape(count, height, width)
        u_plane = cpu_words[:, y_samples:y_samples + chroma_samples].reshape(
            count, height, chroma_width)
        v_plane = cpu_words[:, y_samples + chroma_samples:y_samples + 2 * chroma_samples].reshape(
            count, height, chroma_width)
        a_plane = cpu_words[:, y_samples + 2 * chroma_samples:].reshape(count, height, width)
        if chroma_width != width:
            u_plane = np.repeat(u_plane, 2, axis=2)
            v_plane = np.repeat(v_plane, 2, axis=2)
        reference = np.stack((a_plane, y_plane, u_plane, v_plane), axis=3)
        delta = np.abs(dx_words.astype(np.int32) - reference.astype(np.int32))
        return dx_words, y_plane, u_plane, v_plane, a_plane, delta

    if streaming:
        frames = int(info['nb_frames'])
        if frames <= 0:
            raise RuntimeError(f'ストリームのフレーム数が不明: {path}')
        max_by_component = [0, 0, 0, 0]
        mismatches = [0, 0, 0, 0]
        with (out / f'{name}-cpu.log').open('w', encoding='utf-8') as cpu_log, \
             (out / f'{name}-dx11.log').open('w', encoding='utf-8') as dx_log:
            cpu_process = subprocess.Popen([str(arg) for arg in cpu_command + ['-']],
                                           stdout=subprocess.PIPE, stderr=cpu_log)
            dx_process = subprocess.Popen([str(arg) for arg in dx_command + ['fdsink', 'fd=1']],
                                          stdout=subprocess.PIPE, stderr=dx_log, env=env)
            try:
                for _ in range(frames):
                    cpu_bytes = read_exact(cpu_process.stdout, frame_samples * 2)
                    dx_bytes = read_exact(dx_process.stdout, dx_frame_samples * 2)
                    _, _, _, _, _, delta = compare_arrays(
                        np.frombuffer(cpu_bytes, dtype='<u2'),
                        np.frombuffer(dx_bytes, dtype='<u2'), 1)
                    component_max = delta.max(axis=(0, 1, 2))
                    component_mismatches = (delta != 0).sum(axis=(0, 1, 2))
                    max_by_component = [max(max_by_component[i], int(component_max[i]))
                                        for i in range(4)]
                    mismatches = [mismatches[i] + int(component_mismatches[i])
                                  for i in range(4)]
                if cpu_process.stdout.read(1) or dx_process.stdout.read(1):
                    raise RuntimeError(f'想定枚数後に余剰rawデータ: {path}')
                if cpu_process.wait(timeout=30) or dx_process.wait(timeout=30):
                    raise RuntimeError(f'ストリーム処理が失敗: {path}')
            finally:
                if cpu_process.poll() is None:
                    cpu_process.kill()
                if dx_process.poll() is None:
                    dx_process.kill()
                cpu_process.stdout.close()
                dx_process.stdout.close()
    else:
        cpu_raw = temp / 'cpu.raw'
        dx_raw = temp / 'dx11.raw'
        run(cpu_command + ['-y', cpu_raw], out / f'{name}-cpu.log')
        run(dx_command + ['filesink', f'location={dx_raw.as_posix()}'],
            out / f'{name}-dx11.log', env)
        cpu = np.fromfile(cpu_raw, dtype='<u2')
        dx = np.fromfile(dx_raw, dtype='<u2')
        if cpu.size % frame_samples:
            raise RuntimeError(f'CPU rawサイズがフレーム境界でない: {path}')
        frames = cpu.size // frame_samples
        if frames == 0 or dx.size != frames * dx_frame_samples:
            raise RuntimeError(f'DX11 rawサイズが異なる: {path}; CPU={cpu.size}, DX11={dx.size}')
        dx, y, u, v, a, difference = compare_arrays(cpu, dx, frames)
        max_by_component = difference.max(axis=(0, 1, 2)).tolist()
        mismatches = (difference != 0).sum(axis=(0, 1, 2)).tolist()
    if any(value > 1 for value in max_by_component) or max_by_component[0]:
        raise RuntimeError(f'画素差が許容範囲外: {path}; max={max_by_component}')

    vulkan_log = out / f'{name}-vulkan.log'
    run([FFMPEG / 'ffmpeg.exe', '-hide_banner', '-loglevel', 'verbose',
         '-hwaccel', 'vulkan', '-hwaccel_output_format', 'vulkan',
         '-i', path, '-map', '0:v:0', '-f', 'null', 'NUL'], vulkan_log)
    vk_text = vulkan_log.read_text(encoding='utf-8', errors='replace')
    if 'Vulkan decoder initialization successful' not in vk_text or 'pixfmt:vulkan' not in vk_text:
        raise RuntimeError(f'Vulkan GPU復号を確認できない: {path} ({vulkan_log})')
    result = {
        'file': str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path),
        'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'fourcc': info['codec_tag_string'],
        'pixel_format': pixel_format,
        'frames': int(frames),
        'pixels': int(frames * y_samples),
        'max_difference_ayuv': max_by_component,
        'mismatches_ayuv': mismatches,
        'vulkan_gpu_decode': True,
    }
    if rgb:
        tagged = temp / 'bt709.mov'
        rgba_raw = temp / 'rgba64.raw'
        run([FFMPEG / 'ffmpeg.exe', '-hide_banner', '-loglevel', 'error',
             '-i', path, '-map', '0:v:0', '-c:v', 'copy',
             '-color_primaries', 'bt709', '-color_trc', 'bt709',
             '-colorspace', 'bt709', '-y', tagged], out / f'{name}-retag.log')
        run([GST / 'gst-launch-1.0.exe', '-e', '-q',
             'filesrc', f'location={tagged.as_posix()}', '!', 'qtdemux', '!',
             'proresd3d11dec', '!', 'proresd3d11rgb', '!',
             'd3d11download', '!', 'filesink', f'location={rgba_raw.as_posix()}'],
            out / f'{name}-rgb.log', env)
        rgba = np.fromfile(rgba_raw, dtype='<u2')
        if rgba.size != frames * dx_frame_samples:
            raise RuntimeError(f'RGBA64 rawサイズが異なる: {path}')
        rgba = rgba.reshape(frames, height, download_stride_pixels, 4)[:, :, :width, :]
        depth = 12 if '12le' in pixel_format else 10
        expected_alpha = np.rint(a.astype(np.float64) * 65535 / ((1 << depth) - 1))
        alpha_difference = np.abs(rgba[..., 3].astype(np.int32) - expected_alpha.astype(np.int32))
        result['rgba64_alpha_max_difference'] = int(alpha_difference.max())
        result['rgba64_alpha_mismatches'] = int(np.count_nonzero(alpha_difference))
        if result['rgba64_alpha_max_difference'] > 1:
            raise RuntimeError(f'RGBA64 alpha差が1を超える: {path}')
        scale = 4 if depth == 12 else 1
        def expected_rgb_codes(y_code, u_code, v_code):
            yy = (y_code.astype(np.float64) / scale - 64.0) / 876.0
            cb = (u_code.astype(np.float64) / scale - 512.0) / 896.0
            cr = (v_code.astype(np.float64) / scale - 512.0) / 896.0
            channels = np.stack((yy + 1.5748 * cr,
                                 yy - 0.187324 * cb - 0.468124 * cr,
                                 yy + 1.8556 * cb), axis=3)
            return np.rint(np.clip(channels, 0.0, 1.0) * 65535).astype(np.int32)

        def centered_chroma(plane):
            if chroma_width == width:
                return plane
            location = (np.arange(width, dtype=np.float64) - 0.5) * 0.5
            floor = np.floor(location)
            raw_low = floor.astype(np.int32)
            low = np.clip(raw_low, 0, chroma_width - 1)
            high = np.clip(raw_low + 1, 0, chroma_width - 1)
            fraction = location - floor
            even = plane[:, :, ::2].astype(np.float64)
            return even[:, :, low] * (1.0 - fraction) + even[:, :, high] * fraction

        expected_rgb = expected_rgb_codes(dx[..., 1], centered_chroma(dx[..., 2]),
                                          centered_chroma(dx[..., 3]))
        rgb_difference = np.abs(rgba[..., :3].astype(np.int32) - expected_rgb)
        result['rgba64_rgb_formula_max_difference'] = int(rgb_difference.max())
        result['rgba64_rgb_formula_mismatches'] = int(np.count_nonzero(rgb_difference))
        cpu_rgb = expected_rgb_codes(y, centered_chroma(u), centered_chroma(v))
        result['rgba64_rgb_cpu_formula_max_difference'] = int(
            np.abs(rgba[..., :3].astype(np.int32) - cpu_rgb).max())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--rgb', action='store_true', help='BT.709タグ付きRGBA64出力も比較')
    parser.add_argument('--streaming', action='store_true', help='大容量素材をフレーム単位で照合')
    parser.add_argument('--out', type=Path, default=ROOT / 'results/m3-alpha-comparison')
    args = parser.parse_args()
    if args.streaming and args.rgb:
        parser.error('--streamingと--rgbの同時使用は未対応')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []
    with tempfile.TemporaryDirectory(prefix='prores-m3-alpha-') as temporary:
        temp = Path(temporary)
        for path in args.inputs:
            item = compare(path.resolve(), out, temp, args.rgb, args.streaming)
            results.append(item)
            print(f"{Path(item['file']).name}: {item['frames']}枚、最大差{item['max_difference_ayuv']}")
    summary = {
        'files': len(results),
        'frames': sum(item['frames'] for item in results),
        'pixels': sum(item['pixels'] for item in results),
        'max_difference_ayuv': [max(item['max_difference_ayuv'][i] for item in results)
                                for i in range(4)],
        'fixtures': results,
    }
    if args.rgb:
        summary['rgba64_alpha_max_difference'] = max(
            item['rgba64_alpha_max_difference'] for item in results)
        summary['rgba64_rgb_formula_max_difference'] = max(
            item['rgba64_rgb_formula_max_difference'] for item in results)
    (out / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n',
                                      encoding='utf-8')
    print(json.dumps({key: value for key, value in summary.items() if key != 'fixtures'},
                     ensure_ascii=False))


if __name__ == '__main__':
    main()
