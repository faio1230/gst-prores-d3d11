"""同一デコーダーのCPU/Vulkan出力を同じビット深度で全サンプル比較する。"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'

def main():
    p = argparse.ArgumentParser()
    p.add_argument('input', type=Path)
    p.add_argument('--frames', type=int, default=30)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--bin', type=Path, default=BIN)
    p.add_argument('--threads', type=int, default=0)
    p.add_argument('--avoid-host-import', action='store_true')
    p.add_argument('--native', action='store_true', help='GPU完了待ちを行う独自計測器で比較')
    a = p.parse_args()
    if a.frames < 1:
        p.error('frames must be positive')
    probe = json.loads(subprocess.check_output([str(a.bin / 'ffprobe.exe'), '-v', 'error', '-select_streams', 'v:0', '-show_streams', '-of', 'json', str(a.input)]))['streams'][0]
    fmt = probe['pix_fmt']
    if probe['codec_name'] != 'prores' or fmt not in ('yuv422p10le','yuv444p12le','yuva444p12le','yuv444p10le','yuva444p10le'):
        raise ValueError(f'unsupported comparison format: {fmt}')
    w, h = probe['width'], probe['height']
    depth = 12 if '12' in fmt else 10
    sizes = [w*h, ((w+1)//2 if '422' in fmt else w)*h, ((w+1)//2 if '422' in fmt else w)*h]
    if fmt.startswith('yuva'):
        sizes.append(w*h)
    frame_samples = sum(sizes)
    commands = []
    with tempfile.TemporaryDirectory(dir=ROOT / 'media') as temp:
        files = []
        for mode in ('cpu', 'vulkan'):
            dest = Path(temp) / f'{mode}.raw'
            cmd = [str(a.bin / 'ffmpeg.exe'), '-v', 'error', '-xerror', '-y']
            if mode == 'vulkan':
                if a.avoid_host_import:
                    cmd += ['-init_hw_device', 'vulkan=vk:0,avoid_host_import=1', '-hwaccel_device', 'vk']
                cmd += ['-hwaccel', 'vulkan', '-hwaccel_output_format', 'vulkan']
            cmd += ['-threads',str(a.threads),'-c:v', 'prores', '-i', str(a.input), '-map', '0:v:0', '-frames:v', str(a.frames)]
            if mode == 'vulkan':
                cmd += ['-vf', f'hwdownload,format={fmt}']
            cmd += ['-c:v','rawvideo','-pix_fmt',fmt,'-f','rawvideo',str(dest)]
            env = dict(os.environ, PATH=str(a.bin.resolve())+os.pathsep+os.environ['PATH'])
            if a.native:
                cmd = [str(ROOT/'build/vs18/Release/prores_bench.exe'),str(a.input), 'cpu' if mode=='cpu' else 'download',str(Path(temp)/f'{mode}.csv'),'1',str(a.threads),'0',str(dest),str(a.frames)]
            subprocess.run(cmd, check=True, timeout=180, env=env)
            commands.append(cmd)
            files.append(dest)
        if files[0].stat().st_size != files[1].stat().st_size:
            raise ValueError('frame size/count mismatch')
        actual = files[0].stat().st_size // (frame_samples*2)
        if actual < a.frames or actual*frame_samples*2 != files[0].stat().st_size:
            raise ValueError('incomplete requested frames')
        cpu = np.memmap(files[0], dtype='<u2', mode='r', shape=(actual,frame_samples))
        vk = np.memmap(files[1], dtype='<u2', mode='r', shape=(actual,frame_samples))
        planes=[]
        offset=0
        for name, size in zip('YUVA', sizes):
            x = cpu[:,offset:offset+size].astype(np.int32)
            y = vk[:,offset:offset+size].astype(np.int32)
            diff = x-y
            mse = float(np.mean(diff.astype(np.float64)**2))
            planes.append(dict(plane=name,max_abs=int(np.max(np.abs(diff))),mae=float(np.mean(np.abs(diff))),mse=mse,psnr_db=10*math.log10(((1<<depth)-1)**2/mse) if mse else None,exact=bool(np.all(diff==0)),different_samples=int(np.count_nonzero(diff)),samples=diff.size,cpu_min=int(x.min()),cpu_max=int(x.max()),vulkan_min=int(y.min()),vulkan_max=int(y.max())))
            planes[-1]['per_frame']=[dict(frame=i,max_abs=int(np.max(np.abs(diff[i]))),different_samples=int(np.count_nonzero(diff[i])),vulkan_zero_samples=int(np.count_nonzero(y[i]==0))) for i in range(actual)]
            offset += size
        del cpu,vk
        hashes={f.stem: hashlib.file_digest(f.open('rb'),'sha256').hexdigest() for f in files}
    result=dict(input=str(a.input),input_sha256=hashlib.file_digest(a.input.open('rb'),'sha256').hexdigest(),format=fmt,depth=depth,frames=actual,probe=probe,planes=planes,output_hashes=hashes,commands=commands)
    a.out.parent.mkdir(parents=True,exist_ok=True)
    a.out.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(planes,indent=2))

if __name__=='__main__':
    main()
