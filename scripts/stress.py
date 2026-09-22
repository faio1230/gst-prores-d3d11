"""複数プロセスによるデコードとD3D11合成の競合を測る。共通コンポジターではない。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser()
    p.add_argument('input',type=Path)
    p.add_argument('--count',type=int,default=2)
    p.add_argument('--loops',type=int,default=20)
    p.add_argument('--layers',type=int,default=4)
    p.add_argument('--mode',choices=['cpu-d3d11','interop'],default='interop')
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.count<1 or a.count>8:
        p.error('count must be 1..8')
    a.out.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,PATH=str(ROOT/'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin')+os.pathsep+os.environ['PATH'],PRORES_COMPOSE_LAYERS=str(a.layers))
    processes=[]
    gpu_log=(a.out/'gpu.csv').open('w')
    monitor=None
    try:
        monitor=subprocess.Popen(['nvidia-smi','--query-gpu=timestamp,index,utilization.gpu,utilization.memory,memory.used,power.draw','--format=csv','-lms','200'],stdout=gpu_log,stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        pass
    start=time.perf_counter()
    try:
        for i in range(a.count):
            log=(a.out/f'{i}.log').open('w')
            cmd=[str(ROOT/'build/vs18/Release/prores_bench.exe'),str(a.input.resolve()),a.mode,str(a.out/f'{i}.csv'),str(a.loops),'0','30']
            process=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=log,text=True)
            processes.append((process,log,cmd))
        rows=[]
        for process,log,cmd in processes:
            stdout,_=process.communicate(timeout=300)
            log.close()
            if process.returncode:
                raise RuntimeError('stress process failed; see log')
            row=json.loads(stdout);row['command']=cmd;rows.append(row)
    finally:
        for process,log,_ in processes:
            if process.poll() is None:
                process.kill();process.wait()
            log.close()
        if monitor is not None:
            monitor.terminate();monitor.wait(timeout=10)
        gpu_log.close()
    result=dict(input=str(a.input),mode=a.mode,count=a.count,layers=a.layers,wall_seconds=time.perf_counter()-start,processes=rows)
    (a.out/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))

if __name__=='__main__':
    main()
