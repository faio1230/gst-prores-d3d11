"""保存済みの成功・失敗結果から日本語レポートを作る。未測定値は補完しない。"""
import csv
import json
from pathlib import Path
from statistics import median

ROOT=Path(__file__).resolve().parents[1]

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def gpu_stats(folder, source, mode):
    util=[];memory=[]
    for path in folder.glob(f'{Path(source).stem}-{mode}-*.gpu.csv'):
        for row in list(csv.reader(path.open(encoding='utf-8-sig')))[1:]:
            if len(row)<5:continue
            try:
                util.append(float(row[2].strip().split()[0]));memory.append(float(row[4].strip().split()[0]))
            except ValueError:pass
    return (f'{median(util):.0f} / {max(memory):.0f} / {len(util)}' if util else '未取得')

def main():
    baseline=read(ROOT/'results/baseline-final/summary.json')
    lines=['# 実機測定結果','', '測定日：2026-09-22。RTX 3070 8 GiB / NVIDIA 591.86、i9-12900、Windows 11、FFmpeg n8.1.3-20260921。合成素材のみ。数値は本PC・今回の負荷条件に限定する。','',
           '**判断：4KのGPU受け渡し経路は継続評価する価値がある。製品用GStreamerプラグイン化と完全DX11デコーダーの採用は保留。**','',
           '## デコードとD3D11受け渡し','',
           '同じ180フレームを12周、各周30枚を定常統計から除外、3回起動。値は成功試行の中央値。p99とseek p95は各試行で算出した値の中央値。成功数と失敗数を併記する。', '',
           '| 素材 | モード | 成功/試行 | 定常fps | 供給p99 ms | 初回完了 ms | 先頭seek p95 ms | CPU全体 % | peak WS MiB |',
           '|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for source in sorted({r['input'] for r in baseline}):
        for mode in ['cpu','vulkan','download','cpu-d3d11','download-d3d11','interop']:
            allrows=[r for r in baseline if r['input']==source and r['mode']==mode]
            rows=[r for r in allrows if not r['returncode']]
            if not rows:continue
            values=[median(r[k] for r in rows) for k in ['steady_fps','interval_p99_ms','first_completed_ms','seek_first_p95_ms','cpu_machine_percent','peak_working_set_mib']]
            lines.append(f'| {Path(source).stem} | {mode} | {len(rows)}/{len(allrows)} | '+' | '.join(f'{x:.2f}' for x in values)+' |')
    lines += ['', 'interopは**プレーンごとにGPUコピー2回**。定常画素転送はCPUを通らない。初回完了にはinteropの読み戻しQAを含む。コピー削除やVSync表示の最大fpsとして扱わない。','',
              '## GPU全体の観測値','', 'nvidia-smi、200 ms間隔。成功/失敗試行の実行中サンプル全体を含む。特に停止試行のある1080p Vulkanは低負荷待機を長く含む。プロセス専用負荷/VRAM値ではない。','',
              '| 素材 | モード | GPU使用率中央値 % / device memory.used最大 MiB / サンプル数 |','|---|---|---:|']
    for source in sorted({r['input'] for r in baseline}):
        for mode in ['cpu','vulkan','download','cpu-d3d11','download-d3d11','interop']:
            lines.append(f'| {Path(source).stem} | {mode} | {gpu_stats(ROOT/"results/baseline-final",source,mode)} |')
    composition=ROOT/'results/compose-final/summary.json'
    if composition.exists():
        comp=read(composition)
        lines += ['', '## D3D11合成込み', '', '同じフレームをBT.709 limited→RGBA16Fへ変換し、4層draw/blendするオフスクリーン負荷モデル。3試行の中央値。画面へのPresentと表示同期は含まない。','',
                  '| 素材 | モード | 成功/試行 | fps | 供給p99 ms | CPU全体 % |', '|---|---|---:|---:|---:|---:|']
        for source in sorted({r['input'] for r in comp}):
            for mode in ['cpu-d3d11','interop']:
                allrows=[r for r in comp if r['input']==source and r['mode']==mode]
                rows=[r for r in allrows if not r['returncode']]
                if rows:
                    lines.append(f'| {Path(source).stem} | {mode} | {len(rows)}/{len(allrows)} | {median(r["steady_fps"] for r in rows):.2f} | {median(r["interval_p99_ms"] for r in rows):.2f} | {median(r["cpu_machine_percent"] for r in rows):.2f} |')
    lines += ['', '## 同時デコードによる競合', '', '4K、2プロセス、各プロセスに4層D3D11合成、20周。単一コンポジターへの2入力合成ではない。以下は1回の競合試験であり反復中央値ではない。','', '| モード | 各プロセスfps | 各プロセス供給p99 ms |', '|---|---:|---:|']
    for name in ['stress-cpu','stress-interop']:
        path=ROOT/'results'/name/'summary.json'
        if path.exists():
            data=read(path)
            lines.append(f'| {data["mode"]} | '+', '.join(f'{r["steady_fps"]:.2f}' for r in data['processes'])+' | '+', '.join(f'{r["interval_p99_ms"]:.2f}' for r in data['processes'])+' |')
    lines += ['', '## 画素比較', '', 'GPU完了待ちを行う独自計測器、先頭30フレーム。CPUと同じpixel formatでプレーンごとに比較。画質検査時のraw書き込みfpsは性能統計へ含めない。','', '| 素材 | プレーン | 最大差（元の階調単位） | MAE | PSNR dB | 完全一致 |', '|---|---|---:|---:|---:|---|']
    for filename in ['quality-hq-native-final.json','quality-alpha-native-final.json','quality-4k-native-final.json']:
        path=ROOT/'results'/filename
        if not path.exists():continue
        data=read(path)
        for plane in data['planes']:
            psnr='∞' if plane['psnr_db'] is None else f'{plane["psnr_db"]:.2f}'
            lines.append(f'| {Path(data["input"]).stem} ({data["depth"]} bit) | {plane["plane"]} | {plane["max_abs"]} | {plane["mae"]:.6f} | {psnr} | {"はい" if plane["exact"] else "いいえ"} |')
    failures=[r for r in baseline if r['returncode']]
    lines += ['', '## 採用を保留する理由と残課題', '',
              f'- baselineの失敗は{len(failures)}件。1080p Vulkanの3回目は初回フレーム前に4分超停止し、対象の検証プロセスのみを終了した。終了コード4294967295はこの終了操作によるものでFFmpeg固有エラー番号ではない。`results/vulkan-startup-stall.json` に記録。発生率の推定には試行不足。',
              '- FFmpeg CLI + hwdownloadでは4444の輝度が0を含む大きな不一致を観測。FFmpeg9、threads=1、avoid_host_import=1でも残った。独自計測器の5フレーム試験では最大1階調差だったが、30フレームへ拡大すると大きな不一致が再現した。追加のthreads=1検査では30枚中20枚でYプレーン全体が0。GPU完了待ちだけで解決したとは言えず原因は未確定。[再現情報](不一致の再現.md)を参照。',
              '- 422にはGPU完了待ち後も最大31階調の差が残る。差の許容値は未合意で、IDCTの実装差だけとは断定していない。4444のalphaは選定30フレームで一致したが、輝度破損があるため4444対応済みとはしない。',
              '- 受け渡し前後は初回フレームの全プレーンで完全一致。合成は初回289点をCPU数式と照合。全フレームの色管理、HDR、実モニター表示は未検証。',
              '- 実素材、Proxy/LT/Standard/XQ、interlaced、ネイティブ12 bit入力、任意seek、長時間安定性、Intel/AMD、複数GPU、device lost、GStreamerイベント/メモリ交渉は未検証。',
              '- DX11ではSM5ビット抽出8,192件が一致、8×8逆DCTのfloat基礎試験が成功。完全なDX11 ProResデコーダーやその速度が確認されたわけではない。',
              '', '再実行手順は [ビルドと実行](ビルドと実行.md)、統計の定義とコピー/同期の説明は [測定仕様](測定仕様.md) を参照。基になるCSV/JSON/ログは `results/` に保存している。', '']
    (ROOT/'docs/測定結果.md').write_text('\n'.join(lines),encoding='utf-8')

if __name__=='__main__':main()
