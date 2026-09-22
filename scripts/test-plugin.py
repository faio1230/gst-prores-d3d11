"""実際のGStreamer要素の契約と画素を検査する。各子プロセスには時間上限を設ける。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--gst-root', type=Path, default=Path('C:/Program Files/gstreamer/1.0/msvc_x86_64'))
    p.add_argument('--out', type=Path, default=ROOT/'results/plugin')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    media = ROOT/'media'
    ff = ROOT/'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
    env = dict(os.environ, PATH=str(ff)+os.pathsep+str(a.gst_root/'bin')+os.pathsep+os.environ['PATH'],
               GST_PLUGIN_PATH=str(ROOT/'build/vs18/plugins/Release'),
               GST_REGISTRY=str(ROOT/'build/plugin-test-registry.bin'), GST_DEBUG_NO_COLOR='1')
    tests = []
    report = dict(passed=False, tests=tests)

    def run(name, command, timeout=90):
        command = list(map(str, command))
        entry = dict(name=name, command=command)
        tests.append(entry)
        started = time.perf_counter()
        with (out/f'{name}.stdout').open('wb') as stdout, (out/f'{name}.log').open('wb') as stderr:
            try:
                proc = subprocess.run(command, cwd=ROOT, env=env, stdout=stdout, stderr=stderr, timeout=timeout)
                entry['returncode'] = proc.returncode
            except subprocess.TimeoutExpired:
                entry['timeout_seconds'] = timeout
                raise
            finally:
                entry['process_wall_seconds_including_startup'] = time.perf_counter()-started
        if proc.returncode:
            raise RuntimeError(f'{name}: exit {proc.returncode}; see {out/name}.log')
        print(f'PASS {name}', flush=True)

    try:
        launch = a.gst_root/'bin/gst-launch-1.0.exe'
        run('inspect', [a.gst_root/'bin/gst-inspect-1.0.exe', 'proresvkdec'])
        run('lifecycle', [ROOT/'build/vs18/Release/plugin_smoke.exe', media/'synthetic-1080p60-hq.mov', media/'plugin-first30.raw'])
        report['lifecycle'] = json.loads((out/'lifecycle.stdout').read_text(encoding='utf-8'))
        for resolution in ('1080', '2160'):
            # 起動込みの疎通時間。定常fpsのベンチマークと混同しない。
            run(f'launch-{resolution}', [launch, '-e', '-v', 'filesrc', f'location={(media/f"synthetic-{resolution}p60-hq.mov").as_posix()}',
                '!', 'qtdemux', '!', 'proresvkdec', '!', 'fakesink', 'sync=false'])
        run('convert', [launch, '-e', '-v', 'filesrc', f'location={(media/"synthetic-1080p60-hq.mov").as_posix()}',
            '!', 'qtdemux', '!', 'proresvkdec', '!', 'videoconvert', '!', 'video/x-raw,format=BGRA', '!', 'fakesink', 'sync=false'])
        for profile, name in enumerate(('proxy', 'lt', 'standard', 'hq')):
            fixture = media/f'plugin-{name}.mov'
            run(f'generate-{name}', [ff/'ffmpeg.exe', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                'testsrc2=size=320x180:rate=60', '-frames:v', '30', '-c:v', 'prores_ks', '-profile:v', profile,
                '-pix_fmt', 'yuv422p10le', fixture])
            run(f'profile-{name}', [launch, '-e', '-v', 'filesrc', f'location={fixture.as_posix()}', '!', 'qtdemux',
                '!', 'proresvkdec', '!', 'fakesink', 'sync=false'])
        reference = media/'plugin-reference-vulkan.raw'
        run('reference-vulkan', [ROOT/'build/vs18/Release/prores_bench.exe', media/'synthetic-1080p60-hq.mov',
            'download', out/'reference-vulkan.csv', '1', '1', '0', reference, '30'])
        plugin = media/'plugin-first30.raw'
        expected_size = 1920*1080*4*30
        hashes = dict(plugin=sha(plugin), vulkan_reference=sha(reference), input=sha(media/'synthetic-1080p60-hq.mov'))
        report['pixels'] = dict(frames=30, width=1920, height=1080, format='yuv422p10le', hashes=hashes,
            byte_count=plugin.stat().st_size, exact=hashes['plugin']==hashes['vulkan_reference'])
        if plugin.stat().st_size != expected_size or reference.stat().st_size != expected_size or not report['pixels']['exact']:
            raise RuntimeError('プラグインと単体Vulkan出力の画素またはフレーム数が不一致')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        (out/'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if report['passed'] else 1

if __name__ == '__main__':
    sys.exit(main())
