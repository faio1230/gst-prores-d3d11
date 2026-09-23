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

後続の標準sink・実写4K60検査では、`Present(0)`成功後のOS未表示32件とDWMのflip消費欠番32件を3試行各1441行へ完全対応づけた。これは独立swap chainの`Present(1)`比較を標準sinkの解決策と認定するものではない。キュー置換とDWM内部の破棄を分けるWin32K/DxgKrnl証拠は未取得で、詳細は[実素材と表示検証](実素材と表示検証.md)に記録した。

## 標準sink内の同期値だけを変えた診断

GStreamer 1.28.2の[Win32実装](https://github.com/GStreamer/gstreamer/blob/1.28.2/subprojects/gst-plugins-bad/sys/d3d11/gstd3d11window_win32.cpp#L1206-L1238)は`emit-present=true`時、dirty rectなしで`Present1(0, flags, ...)`する。検査プログラム`d3d11_display_bench`だけに、[非公開window class](https://github.com/GStreamer/gstreamer/blob/1.28.2/subprojects/gst-plugins-bad/sys/d3d11/gstd3d11window.h#L68-L135)の`present`仮想関数を一時差替えする`present-sync1`を追加。同じswap chain・flags・空のpresent parametersで`Present1(1, ...)`を呼び、pipelineをNULLへ戻した後に元の関数へ復元する。検査本体はプラグイン版・型・ABIサイズ、起動wrapperはDLLのSHA256一致を検査するが、非公開ABI依存のため**診断専用であり製品実装・SDK改変ではない**。preroll中の最初のPresentは切替前なので比較の内側から除外する。窓を閉じる操作は元実装と同等に処理していないため、本診断中は検査窓を閉じない。

DJI公式REC.709実写4K60、480枚×3周、RTX 3070／60Hz／GStreamer 1.28.2で、`proresd3d11dec ! proresd3d11rgb ! d3d11videosink`を使用。両条件とも`--preroll --native-rgb --trace-sink-return --decoder-no-qos --settle-ms 150`を指定し、EOS直後のswap chain破棄で最後の2枚のPresentMon記録が確定しない測定誤差を除いた。最前面試験だけ`--topmost-window`を追加し、順序は0→1、1→0、0→1。非最前面は0→1、1→0の2組。同期値1の最初の呼出しでだけswap chain interfaceを確認し、毎フレームのCOM照会が表示位相へ影響する交絡を除いた。各試行ともGStreamerは1440/1440枚、drop／QoS 0、PresentMonは1441行、PTS内側1434枚の捕捉漏れ0。全行の`SyncInterval`と`PresentFlags`は指定値と0で、同期値1のhookは各1442回成功、COM照会は各1回。GStreamerの製品プラグインには手を加えていない。

| ウィンドウ条件 | 同期値0：内側OS未表示 | 同期値1：内側OS未表示 | 最大OS表示間隔 |
|---|---:|---:|---|
| 検査窓だけ最前面、3組 | **133+125+124 / 4302** | **0 / 4302** | 0は33.38～33.42ms、1は21.64～21.67ms |
| 最前面にしない、2組 | **107+8 / 2868** | **0+5 / 2868** | 試行間の変動が大きい |

最前面条件では同期値1がOS未表示を大幅に減らすという、標準sinkに近いA/B証拠が得られた。一方、非最前面の同期値1では5枚残り、表示条件を問わず無欠落になるわけではない。同期値1を製品プラグインの設定や非公開ABIフックとして採用しない。次は保守可能な標準sink側の変更候補を検討し、非最前面・別素材・長時間・通常QoSでend-to-endとOS実表示を同時検証する。今回の診断は欠落が起こる正確なキュー位置も証明しない。

生CSVと各試行JSON・PTS集計は`results/present-sync-standard-2026-09-24/`。採用集計は`*-direct-*`の試行であり、初期の毎フレームCOM照会入り試行`*-topmost*`／`*-settle*`とは分ける。固定GStreamer `gstd3d11.dll` SHA256は`b6156f2299ab0af570b7935138b1389b5f91c84b9a6e0ed755cd15b2e5aa4992`。再現時は、各試行の前にPresentMon 2.6.0を`--process_name d3d11_display_bench.exe --qpc_time_ms --write_display_metadata --set_circular_buffer_size 32768 --timed 37`で起動し、以下をそれぞれ実行する。

```powershell
python scripts/benchmark-d3d11-display.py media/reference-dji-nature-4k60-rec709-hq.mov --loops 3 --repeats 1 --preroll --native-rgb --trace-sink-return --topmost-window --decoder-no-qos --settle-ms 150 --out results/present-sync-standard-2026-09-24/sync0-direct-a
python scripts/benchmark-d3d11-display.py media/reference-dji-nature-4k60-rec709-hq.mov --loops 3 --repeats 1 --preroll --native-rgb --trace-sink-return --topmost-window --decoder-no-qos --present-sync-interval 1 --settle-ms 150 --out results/present-sync-standard-2026-09-24/sync1-direct-a
python scripts/summarize-presentmon-display.py results/present-sync-standard-2026-09-24/presentmon-sync1-direct-a.csv results/present-sync-standard-2026-09-24/sync1-direct-a --out results/present-sync-standard-2026-09-24/sync1-direct-a-summary.json
```

変更後のビルド、DX11係数・画素・D3D11Memory、GStreamer EOS／seek／動的caps／RGBの回帰は`results/verification-present-sync-2026-09-24/`に記録した。初期のsettleなし試行では末尾2枚のPresentMon行が確定せず、内側1枚も未捕捉になったため採用集計には含めない。

### 通常decoder QoSでの追加確認

上のA/BはdecoderのQoS破棄だけを無効化し、OS未表示を独立に測った。通常QoSへ戻し、同じDJI実写4K60・3周・最前面・preroll・専用RGB・EOS後150ms待機で追加した。同期値1の2試行はOS表示時刻なし0/1420・0/1432枚（PTS内側、捕捉漏れ0）だったが、GStreamer出力は1426/1440・1438/1440枚。欠けた14・2枚はdecoder入力後／出力前のQoS破棄で、sink申告dropは0。最初の試行では`Present1(1)` APIが最大110.327msかかり、次のPTSのsink pushも119.2ms待ち、後続にQoS破棄が集中した。同期値0の対照1試行は1440/1440枚、内側OS未表示0/1434枚であり、試行間変動が大きい。**同期値1はOS側を改善しても、通常QoSを含むend-to-end無欠落を満たしていない**。この少数の非交互試行で同期値1が通常QoSに不利と断定しない。

個別PTS・PresentMon・GPU・段階時刻は`results/present-sync-normal-qos-2026-09-24/`。通常QoSの残るsink待ちを復号器内部の負荷と混同せず、同時に短い最大待ちとOS無欠落を満たす保守可能な表示経路が必要である。
