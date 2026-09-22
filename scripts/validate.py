"""終了・失敗経路と計測済みシーク/EOSの記録を確認する。GStreamerイベント試験ではない。"""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser()
    p.add_argument('--results',type=Path,default=ROOT/'results/baseline-final')
    p.add_argument('--allow-failed',action='store_true')
    p.add_argument('--out',type=Path,default=ROOT/'results/validation.json')
    a=p.parse_args()
    reports=[];failures=[]
    for path in a.results.glob('*.json'):
        if path.name=='summary.json':continue
        data=json.loads(path.read_text(encoding='utf-8'))
        if data['returncode']!=0:
            failures.append(dict(file=str(path),returncode=data['returncode']))
            if not a.allow_failed:raise RuntimeError(f'failed result: {path}')
            continue
        rows=list(csv.DictReader(path.with_suffix('.csv').open(newline='')))
        assert len(rows)==data['frames'],path
        epochs={int(r['epoch']) for r in rows}
        first_pts=[]
        for epoch in sorted(epochs):
            points=[int(r['pts']) for r in rows if int(r['epoch'])==epoch]
            assert all(b>a for a,b in zip(points,points[1:])),(path,epoch)
            first_pts.append(points[0])
        assert len(set(first_pts))==1,path
        assert len(epochs)-1==data['seek_count'],path
        assert len({sum(int(r['epoch'])==epoch for r in rows) for epoch in epochs})==1,path
        reports.append(dict(file=str(path),frames=len(rows),epochs=len(epochs),pts_monotonic=True,seek_first_pts_equal=True))
    if not reports:raise RuntimeError('no measurements')
    env=dict(os.environ,PATH=str(ROOT/'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin')+os.pathsep+os.environ['PATH'],PRORES_COMPOSE_LAYERS='0')
    cases=[]
    with tempfile.TemporaryDirectory(dir=ROOT/'media') as tmp:
        tmp=Path(tmp);empty=tmp/'empty.mov';empty.write_bytes(b'');broken=tmp/'truncated.mov'
        with (ROOT/'media/synthetic-1080p60-hq.mov').open('rb') as f:broken.write_bytes(f.read(4096))
        for label,file,mode in [('missing',tmp/'missing.mov','cpu'),('empty',empty,'cpu'),('truncated',broken,'vulkan'),('unknown-mode',empty,'invalid')]:
            run=subprocess.run([str(ROOT/'build/vs18/Release/prores_bench.exe'),str(file),mode,str(tmp/'frames.csv'),'1'],env=env,text=True,capture_output=True,timeout=20)
            assert run.returncode!=0,label
            cases.append(dict(case=label,returncode=run.returncode,stderr=run.stderr))
    result=dict(measurement_checks=reports,failed_measurements=failures,error_cases=cases,gstreamer_events_tested=False,arbitrary_seek_tested=False)
    a.out.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(f'{len(reports)} measurement records and {len(cases)} error cases validated; {len(failures)} failed measurements retained')
if __name__=='__main__':main()
