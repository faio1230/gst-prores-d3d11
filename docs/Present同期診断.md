# DXGI Present同期値の診断（2026-09-24）

標準`d3d11videosink`の実表示では、DXGI Presentが全件`S_OK`でもOS表示時刻なしが残った。GStreamer 1.28.2のsinkには同期値の公開プロパティがないため、製品プラグインとSDKを変更せず、診断専用`d3d11_present_control`を追加した。`proresd3d11dec ! proresd3d11rgb ! appsink`または`d3d11testsrc ! appsink`からRGB10A2 D3D11Memoryを受け、同じdeviceの3バッファflip-discard swap chainへGPU内コピーして`Present(0)`／`Present(1)`する。CPUへ画像は読み戻さない。これを標準sinkの代替製品とはしない。

4K60・各試行480枚。2条件を0→1、1→0、0→1の順に交互実行し、60fps PTSへ時計同期した。診断ウィンドウ自身だけを最前面にした条件では、各480回の可視・非最小化・最前面を記録した。PresentMon 2.6.0の`CPUStartQPCTimeInMs`をCopyResource前～Present復帰のQPC区間へ1対1で帰属し、先頭・末尾各3枚を除外。内側に捕捉漏れがある試行は未表示率から除外する。全て`Composed: Flip`、DXGI結果`S_OK`、同期値は指定どおり、flags=0だった。

| 最前面の入力 | Present(0)：OS表示時刻なし／内側 | Present(1)：OS表示時刻なし／内側 | 備考 |
|---|---:|---:|---|
| DJI公式ProRes 422 HQ実写4K60 | **37/1896**、4試行別28/0/0/9 | **0/1422**、捕捉十分な3試行別0/0/0 | Present(1)の別1試行は内側1枚の捕捉漏れで除外 |
| 復号器なし`d3d11testsrc` 4K60 | **26/1422**、3試行別0/25/1 | **0/1422**、3試行別0/0/0 | 同じ診断swap chain。GPU負荷は実写経路と同一ではない |

最前面ではない初期の交互3組も取得した。実写はPresent(0)で18/1422、Present(1)で14/1422、復号器なしでは328/1422・324/1422と大きく変動した。この初期記録にはウィンドウ状態列がなく、後続の短時間試行で前面ウィンドウとの矩形重なり100%を確認したため、同期値の因果比較には使わない。最前面の結果でもPresent(0)の欠落は試行ごとに変動し、同期値1が常に無欠落と証明したわけではない。

再現手順（RTX 3070、60Hz画面、GStreamer 1.28.2。実写素材の出所・SHA256は[実素材と表示検証](実素材と表示検証.md)）：

```powershell
./scripts/build.ps1
./scripts/benchmark-present-control.ps1 -Repeats 3 -MaxFrames 480 -Topmost -OutDir results/present-control-dji-topmost-4k60-2026-09-24
./scripts/benchmark-present-control.ps1 -Repeats 1 -MaxFrames 480 -Topmost -OutDir results/present-control-dji-topmost-extra-2026-09-24
./scripts/benchmark-present-control.ps1 -Source testsrc -Repeats 3 -MaxFrames 480 -Topmost -OutDir results/present-control-testsrc-topmost-4k60-2026-09-24
python scripts/summarize-present-control.py results/present-control-dji-topmost-4k60-2026-09-24 results/present-control-dji-topmost-extra-2026-09-24 --out results/present-control-dji-topmost-combined-summary.json
python scripts/summarize-present-control.py results/present-control-testsrc-topmost-4k60-2026-09-24
```

実写の追加1組は捕捉不足を補う反復であり、上の実写合算は3組＋追加1組の結果。入力・QPC・PresentMonの生CSV、表示状態、各試行JSONと正式集計を上記の3ディレクトリへ保存した。`results/present-control-dji-topmost-combined-summary.json`が実写合算である。非最前面の探索記録は`results/present-control-dji-4k60-2026-09-24/`と`results/present-control-testsrc-4k60-2026-09-24/`へ分離した。

変更後に`./scripts/test-d3d11-plugin.ps1`と`./scripts/test-dx11.ps1`を通過。EOS・seek・動的解像度caps・D3D11Memory・RGB要素・係数完全一致・画素最大差1を維持した。記録は`results/verification-present-control-2026-09-24/`。

解釈：DXGI API失敗を原因とする説明は否定され、診断swap chainでは同期値0とOS未表示が同時に現れる。ただし独立`appsink`、CPU時計同期、4K→swap chainコピー、自前ウィンドウを使い、標準`d3d11videosink`の待機・QoS・ウィンドウ処理とは異なる。最前面化も製品既定値へ採用しない。次は標準sinkと同等の条件で同期値だけを変える診断を行い、DWMの個別ラッチ／キュー破棄イベントをPTSへ帰属する。実写4K60のend-to-end欠落と短い最大表示間隔を同時に満たすまでは製品採用を保留する。
