"""M5の途中caps変更を単一GStreamerパイプラインで復号し、固定SDK CPUと全画素比較する。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'media/feature-matrix-2026-09-24'
SDK = ROOT / 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
GST = Path('C:/Program Files/gstreamer/1.0/msvc_x86_64/bin')
PLUGIN = ROOT / 'build/vs18/plugins/Release'
SCENARIOS = {
    'color': ['apch-bt601', 'apch-bt709', 'apch-bt2020', 'apch-pq', 'apch-hlg'],
    'resolution': ['apch', 'apch-non16'],
    'odd444': ['ap4h-odd', 'ap4h-non16'],
    'format': ['ap4h-422', 'ap4h-no-alpha', 'ap4h-alpha16'],
}
FORMATS = {'yuv422p10le': 'I422_10LE', 'yuv422p12le': 'I422_12LE',
           'yuv444p10le': 'Y444_10LE', 'yuv444p12le': 'Y444_12LE',
           'yuva444p10le': 'AYUV64', 'yuva444p12le': 'AYUV64'}
COLORIMETRY = {'apch-bt601': 'bt601', 'apch-bt709': 'bt709',
               'apch-bt2020': '2:6:5:7', 'apch-pq': 'bt2100-pq',
               'apch-hlg': 'bt2100-hlg'}
CAPS = re.compile(r'GstProresD3D11Dec:[^\n]+\.GstPad:src: caps = [^\n]+'
                  r'format=\(string\)(\w+), width=\(int\)(\d+), height=\(int\)(\d+)'
                  r'[^\n]+colorimetry=\(string\)([^,\s]+)')


def run(command, *, env=None):
    completed = subprocess.run([str(value) for value in command], capture_output=True,
                               env=env, timeout=120, check=False)
    if completed.returncode:
        raise RuntimeError(f'{command[0]} exit={completed.returncode}: '
                           f'{completed.stderr.decode(errors="replace")[-800:]}')
    return completed.stdout


def probe(path):
    stream = json.loads(run([SDK / 'ffprobe.exe', '-v', 'error', '-select_streams', 'v:0',
                             '-show_entries', 'stream=pix_fmt,width,height,nb_frames',
                             '-of', 'json', path]))['streams'][0]
    return {key: int(stream[key]) if key != 'pix_fmt' else stream[key]
            for key in ('pix_fmt', 'width', 'height', 'nb_frames')}


def compare_frame(info, frame_index, actual_path, expected):
    width, height, fmt = info['width'], info['height'], info['pix_fmt']
    depth = 12 if '12le' in fmt else 10
    chroma_width = width // 2 if '422' in fmt else width
    source = np.frombuffer(expected, dtype='<u2')
    y_count, uv_count = width * height, chroma_width * height
    frame_words = 2 * (y_count + uv_count) if 'yuva' in fmt else y_count + 2 * uv_count
    if source.size != info['nb_frames'] * frame_words:
        raise RuntimeError(f'CPU rawサイズ不一致: {actual_path}')
    source = source.reshape(info['nb_frames'], frame_words)[frame_index]
    planes = [source[:y_count].reshape(height, width)]
    planes += [source[y_count + offset * uv_count:y_count + (offset + 1) * uv_count]
               .reshape(height, chroma_width) for offset in range(2)]
    actual = np.fromfile(actual_path, dtype='<u2')
    if 'yuva' in fmt:
        # AYUV64はGStreamer標準の16bit UNORM。A,Y,U,V順の4成分。
        stride = (width + 15) & ~15
        if actual.size != stride * height * 4:
            raise RuntimeError(f'AYUV64サイズ不一致: {actual_path}: {actual.size}')
        ayuv = actual.reshape(height, stride, 4)[:, :width, :]
        planes[1] = np.repeat(planes[1], 2, axis=1) if chroma_width != width else planes[1]
        planes[2] = np.repeat(planes[2], 2, axis=1) if chroma_width != width else planes[2]
        differences = [int(np.abs((ayuv[:, :, i + 1].astype(np.uint32) >> (16 - depth))
                                  .astype(np.int32) - plane.astype(np.int32)).max())
                       for i, plane in enumerate(planes)]
        alpha = source[y_count + 2 * uv_count:].reshape(height, width)
        alpha_diff = np.abs((ayuv[:, :, 0].astype(np.uint32) >> (16 - depth))
                            .astype(np.int32) - alpha.astype(np.int32))
        differences.append(int(alpha_diff.max()))
    else:
        widths = (width, chroma_width, chroma_width)
        strides = tuple((plane_width + 1) & ~1 for plane_width in widths)
        if actual.size != sum(stride * height for stride in strides):
            raise RuntimeError(f'YUVサイズ不一致: {actual_path}: {actual.size}')
        differences = []
        offset = 0
        for plane, plane_width, stride in zip(planes, widths, strides):
            decoded = actual[offset:offset + stride * height].reshape(height, stride)[:, :plane_width]
            differences.append(int(np.abs(decoded.astype(np.int32) - plane.astype(np.int32)).max()))
            offset += stride * height
    return differences


def scenario(name, directory, log_directory):
    sources = [FIXTURES / f'{stem}.mov' for stem in SCENARIOS[name]]
    infos = [probe(source) for source in sources]
    env = os.environ.copy()
    env['PATH'] = str(GST) + os.pathsep + env.get('PATH', '')
    env['GST_PLUGIN_PATH'] = str(PLUGIN)
    env['GST_REGISTRY'] = str(ROOT / 'build/vs18/plugin-real-quality-registry.bin')
    command = [GST / 'gst-launch-1.0.exe', '-e', '-v', 'concat', 'name=c', '!',
               'proresd3d11dec', '!', 'd3d11download', '!', 'video/x-raw', '!',
               'multifilesink', f'location={(directory / (name + "-%02d.raw")).as_posix()}']
    for source in sources:
        command += ['filesrc', f'location={source.as_posix()}', '!', 'qtdemux', '!',
                    'queue', '!', 'c.']
    completed = subprocess.run([str(part) for part in command], capture_output=True,
                               env=env, timeout=120, check=False)
    log = (completed.stdout + completed.stderr).decode(errors='replace')
    (log_directory / f'{name}.log').write_text(log, encoding='utf-8')
    if completed.returncode or 'Got EOS from element "pipeline0"' not in log:
        raise RuntimeError(f'{name}: パイプライン終了異常 exit={completed.returncode}')
    caps = list(dict.fromkeys((fmt, int(width), int(height), color)
                              for fmt, width, height, color in CAPS.findall(log)))
    expected_caps = list(dict.fromkeys((FORMATS[info['pix_fmt']], info['width'],
                                        info['height'], COLORIMETRY.get(source.stem, '2:0:0:0'))
                                   for source, info in zip(sources, infos)))
    if caps != expected_caps:
        raise RuntimeError(f'{name}: caps順序不一致 {caps} != {expected_caps}')
    frame_number = 0
    records = []
    for source, info in zip(sources, infos):
        expected = run([SDK / 'ffmpeg.exe', '-v', 'error', '-xerror', '-threads', '1',
                        '-apply_cropping', '0', '-i', source, '-map', '0:v:0',
                        '-frames:v', str(info['nb_frames']), '-fps_mode', 'passthrough',
                        '-pix_fmt', info['pix_fmt'], '-f', 'rawvideo', '-'])
        maxima = []
        for index in range(info['nb_frames']):
            raw = directory / f'{name}-{frame_number:02d}.raw'
            if not raw.is_file():
                raise RuntimeError(f'{name}: フレーム不足 {raw}')
            maxima.append(compare_frame(info, index, raw, expected))
            frame_number += 1
        largest = [max(row[channel] for row in maxima) for channel in range(len(maxima[0]))]
        records.append({'input': source.relative_to(ROOT).as_posix(),
                        'sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                        'format': info['pix_fmt'], 'width': info['width'],
                        'height': info['height'], 'frames': info['nb_frames'],
                        'max_difference': largest, 'passed': max(largest) <= 1})
    if len(list(directory.glob(f'{name}-*.raw'))) != frame_number:
        raise RuntimeError(f'{name}: 想定外の追加フレーム')
    return {'caps_sequence': [dict(format=fmt, width=width, height=height,
                                   colorimetry=color)
                              for fmt, width, height, color in caps],
            'eos': True, 'frames': frame_number, 'inputs': records,
            'passed': all(item['passed'] for item in records)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--log-dir', type=Path, default=ROOT / 'build/m5-probes')
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='m5-dynamic-', dir=ROOT / 'build') as temp:
        result = {name: scenario(name, Path(temp), args.log_dir) for name in SCENARIOS}
    summary = {'gstreamer': '1.28.2', 'reference': 'fixed FFmpeg SDK CPU',
               'scenarios': result, 'passed': all(item['passed'] for item in result.values())}
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({name: {'frames': item['frames'], 'passed': item['passed']}
                      for name, item in result.items()}, ensure_ascii=False))
    if not summary['passed']:
        raise SystemExit('M5途中変更の全画素比較が不合格')


if __name__ == '__main__':
    main()
