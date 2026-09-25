"""OSS公開前回帰のbuild内判定値を、公開用の最小JSONへ集約する。"""
import argparse
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROBES = ROOT / 'build/oss-prepublish'


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def rows(name):
    return read(PROBES / name / 'summary.json')


def coefficient_summary(name, files, frames):
    values = rows(name)
    maximum = max(item['pixel_max_abs'] for item in values)
    result = {
        'files': len(values),
        'frames': sum(item['frames'] for item in values),
        'coefficients_checked': sum(item['coefficients_checked'] for item in values),
        'coefficient_mismatches': sum(item['coefficient_mismatches'] for item in values),
        'shader_errors': sum(item['shader_errors'] for item in values),
        'pixel_max_abs': maximum,
    }
    result['passed'] = (result['files'] == files and result['frames'] == frames and
                        result['coefficient_mismatches'] == 0 and
                        result['shader_errors'] == 0 and maximum <= 1 and
                        all(item['passed'] for item in values))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    coefficients = {
        name: coefficient_summary(directory, files, frames)
        for name, directory, files, frames in (
            ('M1', 'm1-coeff', 11, 420), ('M2', 'm2-coeff', 18, 780),
            ('M3', 'm3-coeff', 24, 48), ('M4', 'm4-coeff', 9, 362),
            ('M5端数', 'm5-coeff', 2, 4))
    }
    m1 = rows('m1-1080') + rows('m1-4k')
    m2 = rows('m2-pixels')
    m3 = rows('m3-alpha')
    m4_alpha = rows('m4-alpha')
    m4_yuv = [read(PROBES / 'm4-yuv' / f'{name}.json')
              for name in ('apch-tff-1080p30', 'apch-bff-1080p30',
                           'apch-tff-4k60', 'apch-bff-4k60')]
    m5 = rows('m5-dynamic')['scenarios']
    odd_rgb = read(PROBES / 'm5-odd-rgb.json')
    rgb = rows('rgb-left')
    hq = rows('hq-public')
    asan = rows('asan')['result']
    smoke = json.loads((ROOT / 'build/oss-final-smoke.log').read_text(
        encoding='utf-8', errors='replace').splitlines()[-1])
    performance = rows('hq-ab')
    plugin = ROOT / 'build/vs18/plugins/Release/gstproresd3d11.dll'
    plugin_hash = hashlib.sha256(plugin.read_bytes()).hexdigest()
    old_plugin = ROOT / 'build/vs18/prores-m5-direct-ab-new/plugins/Release/gstproresd3d11.dll'
    old_hash = hashlib.sha256(old_plugin.read_bytes()).hexdigest()
    maximum_rgb = max(item['rgb_max_abs'] for item in rgb)
    pixel = {
        'M1': {'files': len(m1), 'frames': sum(item['pixel_frames'] for item in m1),
               'max_abs': max(item['pixel_max_abs'] for item in m1)},
        'M2': {'files': len(m2), 'frames': sum(item['frames'] for item in m2),
               'max_abs': max(item['cpu_dx11_pixel_max'] for item in m2)},
        'M3_alpha': {'files': m3['files'], 'frames': m3['frames'],
                     'yuv_max_abs': max(m3['max_difference_ayuv'][1:]),
                     'alpha_max_abs': m3['max_difference_ayuv'][0],
                     'rgba_formula_max_abs': m3['rgba64_rgb_formula_max_difference']},
        'M4_yuv': {'files': len(m4_yuv), 'frames': sum(item['frames'] for item in m4_yuv),
                   'max_abs': max(channel['max_abs'] for item in m4_yuv
                                  for channel in item['channels'])},
        'M4_alpha': {'files': m4_alpha['files'], 'frames': m4_alpha['frames'],
                     'yuv_max_abs': max(m4_alpha['max_difference_ayuv'][1:]),
                     'alpha_max_abs': m4_alpha['max_difference_ayuv'][0]},
        'M5_dynamic': {'scenarios': len(m5),
                       'frames': sum(item['frames'] for item in m5.values()),
                       'passed': all(item['passed'] for item in m5.values())},
        'M5_odd_rgb': {'frames': odd_rgb['frames'],
                       'max_abs': max(channel['max_abs'] for channel in odd_rgb['channels']),
                       'passed': odd_rgb['passed']},
    }
    pixels_passed = (
        pixel['M1'] == {'files': 11, 'frames': 420, 'max_abs': 1} and
        pixel['M2'] == {'files': 18, 'frames': 780, 'max_abs': 1} and
        all(item['passed'] for item in m1) and
        all(item['cpu_dx11_all_planes_passed'] for item in m2) and
        pixel['M3_alpha']['files'] == 24 and pixel['M3_alpha']['frames'] == 48 and
        pixel['M3_alpha']['yuv_max_abs'] <= 1 and
        pixel['M3_alpha']['alpha_max_abs'] == 0 and
        pixel['M3_alpha']['rgba_formula_max_abs'] <= 1 and
        pixel['M4_yuv']['files'] == 4 and pixel['M4_yuv']['frames'] == 180 and
        pixel['M4_yuv']['max_abs'] <= 1 and all(item['passed'] for item in m4_yuv) and
        pixel['M4_alpha']['files'] == 6 and pixel['M4_alpha']['frames'] == 184 and
        pixel['M4_alpha']['yuv_max_abs'] <= 1 and
        pixel['M4_alpha']['alpha_max_abs'] == 0 and
        pixel['M5_dynamic']['scenarios'] == 4 and
        pixel['M5_dynamic']['frames'] == 24 and pixel['M5_dynamic']['passed'] and
        pixel['M5_odd_rgb']['frames'] == 2 and
        pixel['M5_odd_rgb']['max_abs'] <= 1 and pixel['M5_odd_rgb']['passed'])
    rgb_result = {'files': len(rgb), 'frames': sum(item['frames'] for item in rgb),
                  'max_abs': maximum_rgb}
    rgb_result['passed'] = (rgb_result == {'files': 32, 'frames': 1859, 'max_abs': 1} and
                            all(item['passed'] for item in rgb))
    hq_result = {'files': len(hq), 'frames': sum(item['frames'] for item in hq),
                 'passed': len(hq) == 4 and sum(item['frames'] for item in hq) == 779 and
                 all(item['passed'] and item['eos'] and item['direct_d3d11memory']
                     for item in hq)}
    asan_result = {'cases': asan['cases'], 'seeds': asan['seed_count'],
                   'asan_enabled': asan['asan_enabled'],
                   'passed': asan['passed'] and asan['asan_enabled'] and
                             asan['cases'] == 24000 and asan['seed_count'] == 6}
    smoke_result = {'eos_cycles': smoke['eos_cycles'],
                    'flushing_seeks': smoke['flushing_seeks'],
                    'idct_exception_seek': smoke['idct_exception_seek'],
                    'matrix_fallback_zero_and_two': smoke['matrix_fallback_zero_and_two'],
                    'dispatch_bound_8192': smoke['dispatch_bound_8192'],
                    'interlaced_cases': smoke['interlaced_cases'],
                    'passed': smoke['passed'] and smoke['eos_cycles'] == 3 and
                              smoke['flushing_seeks'] == 5 and smoke['idct_exception_seek'] and
                              smoke['matrix_fallback_zero_and_two'] and
                              smoke['dispatch_bound_8192'] and smoke['interlaced_cases'] == 4}
    speed = {'pairs': performance['pairs'], 'old_plugin_sha256': old_hash,
             'new_plugin_sha256': plugin_hash, 'results': performance['results']}
    speed['passed'] = (performance['pairs'] == 4 and
                       performance['old_plugin_sha256'] == old_hash and
                       performance['new_plugin_sha256'] == plugin_hash and
                       len(performance['results']) == 2 and
                       all(item['passed'] and item['change_percent'] >= -3
                           for item in performance['results']))
    normal_gpu_logs = [ROOT / f'build/{name}' for name in
                       ('oss-final-4k444-alpha.log', 'oss-standard-convert.log')]
    normal_gpu = all(path.is_file() and 'ERROR' not in path.read_text(
        encoding='utf-8', errors='replace') for path in normal_gpu_logs)
    result = {'date': '2026-09-25', 'plugin_sha256': plugin_hash,
              'dispatch_8192': {'idct': [65535, 49], 'vld': [12288, 1],
                                'alpha': [4096, 1], 'pack': [1024, 1024],
                                'rgb': [1024, 1024], 'passed': smoke['dispatch_bound_8192']},
              'coefficient': coefficients, 'pixels': pixel,
              'pixels_passed': pixels_passed, 'rgb_left': rgb_result,
              'public_hq': hq_result, 'asan': asan_result, 'smoke': smoke_result,
              'hq_direct_ab': speed, 'normal_gpu_4k444_alpha_and_converter': normal_gpu,
              'd3d11_debug_layer': '未導入のため未実施'}
    result['passed'] = (all(item['passed'] for item in coefficients.values()) and
                        pixels_passed and rgb_result['passed'] and hq_result['passed'] and
                        asan_result['passed'] and smoke_result['passed'] and speed['passed'] and
                        normal_gpu)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': result['passed'], 'rgb_frames': rgb_result['frames'],
                      'hq_fps_change': [item['change_percent'] for item in speed['results']]},
                     ensure_ascii=False))
    if not result['passed']:
        raise SystemExit('OSS公開前回帰のゲート不合格')


if __name__ == '__main__':
    main()
