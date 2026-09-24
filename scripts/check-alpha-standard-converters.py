"""BT.709の標準変換2経路を固定SDK CPUの独立RGBA式と全画素比較する。"""
import argparse
import json
from pathlib import Path
import subprocess

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')[-1000:])
    return result.stdout


def expand(values, depth):
    v = values.astype(np.uint32)
    return ((v << (16 - depth)) | (v >> (2 * depth - 16))).astype(np.uint16)


def rgb_reference(y, u, v):
    yy = (y.astype(np.float64) - 4096.0) / 56064.0
    cb = (u.astype(np.float64) - 32768.0) / 57344.0
    cr = (v.astype(np.float64) - 32768.0) / 57344.0
    rgb = np.stack((yy + 1.5748 * cr,
                    yy - 0.187324 * cb - 0.468124 * cr,
                    yy + 1.8556 * cb), axis=-1)
    return np.rint(np.clip(rgb, 0, 1) * 65535).astype(np.int32)


def inspect(input_path, paths, ayuv_path=None):
    info = json.loads(run([SDK / 'ffprobe.exe', '-v', 'error', '-select_streams', 'v:0',
                           '-show_entries', 'stream=pix_fmt,width,height,nb_frames',
                           '-of', 'json', input_path]))['streams'][0]
    width, height, frames = (int(info[k]) for k in ('width', 'height', 'nb_frames'))
    fmt = info['pix_fmt']
    depth = 10 if '10le' in fmt else 12
    cw = width // 2 if '422' in fmt else width
    ysize, csize = width * height, cw * height
    words_per_frame = ysize * 2 + csize * 2
    packet = run([SDK / 'ffmpeg.exe', '-v', 'error', '-i', input_path, '-map', '0:v:0',
                  '-frames:v', '1', '-c', 'copy', '-f', 'data', '-'])
    if packet[4:8] != b'icpf' or packet[25] & 15 not in (1, 2):
        raise RuntimeError('ProRes alpha modeが不正')
    alpha_bits = 8 if packet[25] & 15 == 1 else 16
    raw = run([SDK / 'ffmpeg.exe', '-v', 'error', '-i', input_path, '-map', '0:v:0',
               '-pix_fmt', fmt, '-f', 'rawvideo', '-'])
    cpu = np.frombuffer(raw, dtype='<u2')
    if cpu.size != frames * words_per_frame:
        raise RuntimeError('CPU rawサイズ不一致')
    cpu = cpu.reshape(frames, words_per_frame)
    y = cpu[:, :ysize].reshape(frames, height, width)
    u = cpu[:, ysize:ysize + csize].reshape(frames, height, cw)
    v = cpu[:, ysize + csize:ysize + 2 * csize].reshape(frames, height, cw)
    a = cpu[:, ysize + 2 * csize:].reshape(frames, height, width)
    if cw != width:
        u, v = np.repeat(u, 2, axis=2), np.repeat(v, 2, axis=2)
    reference = rgb_reference(expand(y, depth), expand(u, depth), expand(v, depth))
    records = []
    stride = (width + 15) // 16 * 16
    ayuv = None
    if ayuv_path:
        ayuv = np.fromfile(ayuv_path, dtype='<u2')
        if ayuv.size != frames * stride * height * 4:
            raise RuntimeError('AYUV64 rawサイズ不一致')
        ayuv = ayuv.reshape(frames, height, stride, 4)[:, :, :width]
        decoded_reference = rgb_reference(ayuv[..., 1], ayuv[..., 2], ayuv[..., 3])
    for method, path in paths:
        actual = np.fromfile(path, dtype='<u2')
        if actual.size != frames * stride * height * 4:
            raise RuntimeError(f'{method} RGBA64 rawサイズ不一致')
        actual = actual.reshape(frames, height, stride, 4)[:, :, :width]
        difference = np.abs(actual[..., :3].astype(np.int32) - reference)
        if alpha_bits == 8:
            source = a.astype(np.uint32) >> (depth - 8)
            expected_alpha = (source << 8) | source
            alpha_error = np.abs(actual[..., 3].astype(np.int32) - expected_alpha)
        else:
            alpha_error = np.abs((actual[..., 3].astype(np.uint32) >> (16 - depth))
                                 .astype(np.int32) - a.astype(np.int32))
        record = {'method': method, 'rgb_max': difference.max(axis=(0, 1, 2)).tolist(),
                        'alpha_max_at_reference_depth': int(alpha_error.max()),
                        'rgb_mean': actual[..., :3].mean(axis=(0, 1, 2)).tolist(),
                        'alpha_mean': float(actual[..., 3].mean()),
                        'alpha_min': int(actual[..., 3].min()),
                        'alpha_max': int(actual[..., 3].max())}
        if ayuv is not None:
            internal_error = np.abs(actual[..., :3].astype(np.int32) - decoded_reference)
            record['rgb_max_from_ayuv'] = internal_error.max(axis=(0, 1, 2)).tolist()
        record['passed'] = (max(record['rgb_max']) <= 256 and
                            record['alpha_max_at_reference_depth'] == 0 and
                            (ayuv is None or max(record['rgb_max_from_ayuv']) <=
                             (1 if method == 'd3d11convert' else 96)))
        records.append(record)
    return {'input': input_path.resolve().relative_to(ROOT).as_posix(),
            'frames': frames, 'alpha_bits': alpha_bits,
            'pixel_format': fmt, 'reference_rgb_mean': reference.mean(axis=(0, 1, 2)).tolist(),
            'results': records, 'passed': all(record['passed'] for record in records)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('gpu_raw', type=Path)
    parser.add_argument('cpu_raw', type=Path)
    parser.add_argument('--ayuv-raw', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    result = inspect(args.input, [('d3d11convert', args.gpu_raw),
                                  ('videoconvert', args.cpu_raw)], args.ayuv_raw)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))
    if not result['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
