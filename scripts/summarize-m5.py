"""build/に残したM5の詳細ログから、文書で参照する単一の判定JSONを作る。"""
import argparse
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROBES = ROOT / 'build/m5-probes'


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def last_json(path):
    return json.loads(path.read_text(encoding='utf-8', errors='replace').splitlines()[-1])


def regression_checks():
    m1 = read_json(PROBES / 'm1/summary-1080.json') + read_json(
        PROBES / 'm1/summary-4k.json')
    m2 = read_json(PROBES / 'm2/summary.json')
    m3 = read_json(PROBES / 'm3-alpha/summary.json')
    m4_alpha = read_json(PROBES / 'm4-alpha/summary.json')
    m4_yuv = [read_json(PROBES / f'{name}-pixels.json') for name in (
        'apch-tff-1080p30', 'apch-bff-1080p30',
        'apch-tff-4k60', 'apch-bff-4k60')]
    public = read_json(PROBES / 'hq-public/summary.json')
    performance = read_json(PROBES / 'hq-ab/summary.json')
    current_plugin = ROOT / 'build/vs18/plugins/Release/gstproresd3d11.dll'
    current_hash = hashlib.sha256(current_plugin.read_bytes()).hexdigest()
    m1_max = max(item['pixel_max_abs'] for item in m1)
    m2_max = max(item['cpu_dx11_pixel_max'] for item in m2)
    m4_yuv_max = max(channel['max_abs'] for item in m4_yuv
                     for channel in item['channels'])
    outcome = {
        'm1': {'files': len(m1), 'frames': sum(item['pixel_frames'] for item in m1),
               'pixel_max': m1_max,
               'passed': len(m1) == 11 and sum(item['pixel_frames'] for item in m1) == 420 and
               m1_max <= 1 and all(item['passed'] and item['coefficient_mismatches'] == 0
                                   and item['shader_errors'] == 0 and item['vulkan_frames'] ==
                                   item['pixel_frames'] for item in m1)},
        'm2': {'files': len(m2), 'frames': sum(item['frames'] for item in m2),
               'pixel_max': m2_max,
               'passed': len(m2) == 18 and sum(item['frames'] for item in m2) == 780 and
               m2_max <= 1 and all(item['cpu_dx11_all_planes_passed'] for item in m2)},
        'm3_alpha': {'files': m3['files'], 'frames': m3['frames'],
                     'ayuv_max': m3['max_difference_ayuv'],
                     'passed': m3['files'] == 24 and m3['frames'] == 48 and
                     m3['max_difference_ayuv'][0] == 0 and
                     max(m3['max_difference_ayuv'][1:]) <= 1},
        'm4_yuv': {'files': len(m4_yuv), 'frames': sum(item['frames'] for item in m4_yuv),
                   'pixel_max': m4_yuv_max,
                   'passed': len(m4_yuv) == 4 and
                   sum(item['frames'] for item in m4_yuv) == 180 and
                   m4_yuv_max <= 1 and all(item['passed'] for item in m4_yuv)},
        'm4_alpha': {'files': m4_alpha['files'], 'frames': m4_alpha['frames'],
                     'ayuv_max': m4_alpha['max_difference_ayuv'],
                     'passed': m4_alpha['files'] == 4 and m4_alpha['frames'] == 180 and
                     m4_alpha['max_difference_ayuv'][0] == 0 and
                     max(m4_alpha['max_difference_ayuv'][1:]) <= 1},
        'public_hq': {'files': len(public), 'frames': sum(item['frames'] for item in public),
                      'passed': len(public) == 4 and
                      sum(item['frames'] for item in public) == 779 and
                      all(item['passed'] and item['eos'] and item['direct_d3d11memory']
                          and item['d3d11memory_frames'] == item['frames'] for item in public)},
        'hq_direct_ab': {'pairs': performance['pairs'],
                         'old_plugin_sha256': performance['old_plugin_sha256'],
                         'new_plugin_sha256': performance['new_plugin_sha256'],
                         'results': performance['results'],
                         'passed': performance['pairs'] == 4 and
                         performance['new_plugin_sha256'] == current_hash and
                         len(performance['results']) == 2 and
                         all(item['passed'] and item['change_percent'] >= -3
                             for item in performance['results'])},
    }
    return outcome


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    dynamic = read_json(PROBES / 'dynamic-summary.json')
    if set(dynamic['scenarios']) != {'color', 'resolution', 'odd444', 'format'}:
        raise RuntimeError('M5シナリオが不足')
    if sum(item['frames'] for item in dynamic['scenarios'].values()) != 24:
        raise RuntimeError('動的パイプラインの比較フレーム数が不足')
    rgb = {}
    for name in ('odd-rgb-comparison', 'old-rgb-regression'):
        result = read_json(PROBES / f'{name}.json')
        maxima = {channel['channel']: channel['max_abs'] for channel in result['channels']}
        rgb[name] = {'input': Path(result['input']).relative_to(ROOT).as_posix(),
                     'source_sha256': result['source_sha256'],
                     'frames': result['frames'], 'max_difference': maxima,
                     'passed': result['passed'] is True and result['frames'] == 2 and
                     max(maxima.values()) <= 1}
    coefficients = {}
    for name in ('apch-non16', 'ap4h-odd'):
        result = last_json(PROBES / f'{name}-coeff-final.log')
        coefficients[name] = {'adapter': result['adapter'],
                              'dimensions': [result['width'], result['height']],
                              'coefficient_mismatches': result['mismatches'],
                              'shader_errors': result['shader_errors'],
                              'pixel_max_difference': max(item['max_abs']
                                                          for item in result['pixel_differences']),
                              'malformed_cases_rejected': result['malformed_cases_rejected'],
                              'passed': result['passed']}
    smoke = last_json(PROBES / 'full-smoke.log')
    asan = read_json(PROBES / 'asan/summary.json')
    converted = {}
    for name in ('apch-bt601', 'apch-bt709', 'apch-bt2020', 'apch-pq', 'apch-hlg'):
        log = (PROBES / f'{name}-convert.log').read_text(encoding='utf-8', errors='replace')
        converted[name] = ('Got EOS from element "pipeline0"' in log and
                           'format=(string)RGBA64_LE' in log)
    dynamic_convert = {}
    for name in ('color', 'resolution', 'format'):
        log = (PROBES / f'dynamic-{name}-convert.log').read_text(
            encoding='utf-8', errors='replace')
        dynamic_convert[name] = ('Got EOS from element "pipeline0"' in log and
                                 'format=(string)RGBA64_LE' in log)
        if name == 'color':
            dynamic_convert[name] &= all(f'colorimetry=(string){color}' in log
                                         for color in ('bt601', 'bt709', '2:6:5:7',
                                                       'bt2100-pq', 'bt2100-hlg'))
    asan_result = asan['result']
    asan_sources = [item['source'] for item in asan['sources']]
    regressions = regression_checks()
    passed = (dynamic['passed'] and all(item['passed'] for item in rgb.values()) and
              all(item['passed'] and item['coefficient_mismatches'] == 0 and
                  item['shader_errors'] == 0 and item['pixel_max_difference'] <= 1
                  for item in coefficients.values()) and
              smoke['passed'] and smoke['eos_cycles'] == 3 and
              smoke['flushing_seeks'] == 4 and smoke['interlaced_cases'] == 4 and
              asan_result['passed'] and asan_result['asan_enabled'] and
              asan_result['cases'] == 24000 and asan_result['seed_count'] == 6 and
              any('ap4h-odd.mov' in item for item in asan_sources) and
              any('apch-non16.mov' in item for item in asan_sources) and
              all(converted.values()) and all(dynamic_convert.values()) and
              all(item['passed'] for item in regressions.values()))
    summary = {'milestone': 'M5', 'reference': 'fixed FFmpeg SDK CPU',
               'dynamic': dynamic['scenarios'], 'rgb': rgb,
               'coefficients': coefficients,
               'regression': {'smoke': smoke, 'asan': asan_result,
                              'asan_sources': asan_sources,
                              'standard_d3d11convert': converted,
                              'dynamic_color_d3d11convert': dynamic_convert,
                              'full': regressions},
               'passed': passed}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n',
                        encoding='utf-8')
    print(json.dumps({'frames': sum(item['frames'] for item in dynamic['scenarios'].values()),
                      'asan_cases': asan_result['cases'], 'passed': passed}, ensure_ascii=False))
    if not passed:
        raise SystemExit('M5判定不合格')


if __name__ == '__main__':
    main()
