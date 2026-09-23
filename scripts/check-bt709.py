"""1フレームYUV422 10bitを明示BT.709式でRGB10A2と比較する診断器。"""
import argparse
from pathlib import Path
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('yuv', type=Path)
    parser.add_argument('rgb', type=Path)
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--height', type=int, required=True)
    args = parser.parse_args()
    width, height = args.width, args.height
    pixels = width * height
    raw = np.fromfile(args.yuv, dtype='<u2', count=pixels * 2)
    rgb = np.fromfile(args.rgb, dtype='<u4', count=pixels).reshape(height, width)
    y = raw[:pixels].reshape(height, width).astype(np.float32)
    u = raw[pixels:pixels + pixels // 2].reshape(height, width // 2).astype(np.float32)
    v = raw[pixels + pixels // 2:].reshape(height, width // 2).astype(np.float32)
    for sampling in ('nearest', 'centered', 'cosited'):
        if sampling == 'nearest':
            cb, cr = np.repeat(u, 2, axis=1), np.repeat(v, 2, axis=1)
        else:
            x = np.arange(width, dtype=np.float32)
            location = (x - .5) / 2 if sampling == 'centered' else x / 2
            base = np.floor(location).astype(np.int32)
            low = np.clip(base, 0, width // 2 - 1)
            high = np.clip(base + 1, 0, width // 2 - 1)
            fraction = np.clip(location - np.floor(location), 0, 1)
            cb = u[:, low] * (1 - fraction) + u[:, high] * fraction
            cr = v[:, low] * (1 - fraction) + v[:, high] * fraction
        yy = (y - 64) / 876
        cb = (cb - 512) / 896
        cr = (cr - 512) / 896
        expected = (
            yy + 1.5748 * cr,
            yy - 0.187324 * cb - 0.468124 * cr,
            yy + 1.8556 * cb,
        )
        print(sampling)
        for channel, shift, predicted in zip('RGB', (0, 10, 20), expected):
            actual = ((rgb >> shift) & 1023).astype(np.int32)
            reference = np.rint(np.clip(predicted * 1023, 0, 1023)).astype(np.int32)
            difference = actual - reference
            print(channel, 'max', np.abs(difference).max(), 'mae',
                  np.abs(difference).mean(), 'bias', difference.mean())


if __name__ == '__main__':
    main()
