"""3段階の中性グレーProRes診断rawをBT.709の期待値と比較する。"""
import argparse
import json
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='compare-d3d11-real.py --diagnostic-dir の出力')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    width, height = 384, 128
    pixels = width * height
    directory = args.directory
    yuv = np.fromfile(directory / 'dx11-i422.raw', dtype='<u2')
    cpu = np.fromfile(directory / 'cpu-rgb10a2.raw', dtype='<u4')
    gpu = np.fromfile(directory / 'gpu-rgb10a2.raw', dtype='<u4')
    if yuv.size != pixels * 2 or cpu.size != pixels or gpu.size != pixels:
        raise ValueError('診断rawの大きさが384x128・1フレームと一致しない')
    y = yuv[:pixels].reshape(height, width)
    u = yuv[pixels:pixels + pixels // 2].reshape(height, width // 2)
    v = yuv[pixels + pixels // 2:].reshape(height, width // 2)
    cpu = cpu.reshape(height, width)
    gpu = gpu.reshape(height, width)
    result = {'input': str(directory), 'format': 'RGB10A2_LE',
              'colorimetry': 'limited BT.709 I422 -> full-range BT.709 RGB',
              'bands': []}
    for index, start in enumerate((0, 128, 256)):
        # 3色の境界では4:2:2の補間が混ざるため、各帯の内側だけを見る。
        x0, x1 = start + 16, start + 112
        y_code = int(np.median(y[:, x0:x1]))
        u_code = int(np.median(u[:, x0 // 2:x1 // 2]))
        v_code = int(np.median(v[:, x0 // 2:x1 // 2]))
        if (y_code, u_code, v_code) != ((64, 504, 940)[index], 512, 512):
            raise ValueError('中性帯のYUV codeが想定と異なる')
        ideal = int(np.rint(np.clip((y_code - 64) * 1023 / 876, 0, 1023)))
        def channels(packed):
            return {name: int(np.median((packed[:, x0:x1] >> shift) & 1023))
                    for name, shift in (('R', 0), ('G', 10), ('B', 20))}
        result['bands'].append({'band': index, 'yuv': [y_code, u_code, v_code],
                                'bt709_expected_rgb': [ideal] * 3,
                                'cpu_rgb': channels(cpu), 'gpu_rgb': channels(gpu)})
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result['bands'], ensure_ascii=False))


if __name__ == '__main__':
    main()
