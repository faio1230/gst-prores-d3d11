"""build/に残したM5の詳細ログから、文書で参照する単一の判定JSONを作る。"""
import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROBES = ROOT / 'build/m5-probes'


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def last_json(path):
    return json.loads(path.read_text(encoding='utf-8', errors='replace').splitlines()[-1])


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
              all(converted.values()) and all(dynamic_convert.values()))
    summary = {'milestone': 'M5', 'reference': 'fixed FFmpeg SDK CPU',
               'dynamic': dynamic['scenarios'], 'rgb': rgb,
               'coefficients': coefficients,
               'regression': {'smoke': smoke, 'asan': asan_result,
                              'asan_sources': asan_sources,
                              'standard_d3d11convert': converted,
                              'dynamic_color_d3d11convert': dynamic_convert},
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
