# 独立したProRes GPUデコーダー：proresvkdec

この文書は**比較・検証用のVulkan中間成果**を説明する。プロジェクトの最終目標は純粋なDX11デコードとGPUメモリのままの出力であり、ここに記載するsystem memory出力版で完成とはしない。以後の主実装と到達条件は[開発計画](プラグイン化計画.md)を参照。

2026-09-22。利用先アプリを外し、GStreamer標準のパイプラインで使える小さな要素を先に作る方針に変更した。`GstVideoDecoder` 派生の独立DLLを実装し、RTX 3070 / NVIDIA 591.86 / Windows 11 / GStreamer 1.28.2 MSVC x64 / FFmpeg 8.1.3で検査した。製品品質・他GPUへの対応は未判定。

## 初版の構成

```text
filesrc → qtdemux → proresvkdec → 通常のGStreamer映像要素
                        │
                        ├ 圧縮フレームをpadding付きAVPacketへコピー
                        ├ FFmpeg ProRes Vulkan computeでGPU復号
                        ├ Vulkan semaphore完了待ち
                        ├ av_hwframe_transfer_dataでCPUへ読み戻し
                        └ GStreamer所有のI422_10LEバッファへ行単位コピー
```

GPU復号後にGPU→CPU転送とCPU上のバッファコピーがある。ゼロコピーではなく、D3D11Memory/VulkanImageを下流へ渡す実装でもない。GPU復号を要求し、ソフトウェアデコードへの自動切替は行わない。Vulkan physical deviceがCPU型の場合も拒否する。FFmpeg自身のフレーム並列処理を避けるため、初版はthread_count=1、slice threadingを使用する。

入力はdemux済みの `video/x-prores`、1バッファに完全な1フレーム。variantは `proxy / lt / standard / hq`、progressive、4:2:2、アルファなし。幅は偶数、幅・高さは16〜8192の範囲で受け付けるが、上限の実機保証はない。実検査は320×180、1920×1080、3840×2160。capsだけでなくフレームヘッダーの形式・長さ・寸法も検査し、不一致をエラーにする。

出力は `video/x-raw,format=I422_10LE`、system memory。10bitのY/U/Vを16bit little endian容器で保持する。色変換・8bit化・スケーリングは行わない。MOVの読み込み、表示、LTC、Spoutは別要素・別アプリの役割。

4444/4444 XQ、アルファ、12bit、インターレース、RAWは対象外。既存の4444検証で輝度異常を確認したため、初版で対応を宣言しない。直接のDX11デコードは未実装で、[SM5基礎検証](DX11実装検証.md)のみ別に保持する。

## ビルドと実行

MSVC C++ビルドツール、Windows SDK、CMake、PowerShell 7、Python 3.11以上、GStreamer MSVC x64のランタイムと開発SDKが必要。GStreamerの既定パスは `C:/Program Files/gstreamer/1.0/msvc_x86_64`。

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
./scripts/generate-media.ps1
. ./scripts/use-plugin.ps1
gst-inspect-1.0 proresvkdec
gst-launch-1.0 -e filesrc location=media/synthetic-1080p60-hq.mov ! qtdemux ! proresvkdec ! fakesink sync=false
gst-launch-1.0 -e filesrc location=media/synthetic-2160p60-hq.mov ! qtdemux ! proresvkdec ! videoconvert ! video/x-raw,format=BGRA ! fakesink sync=false
python scripts/test-plugin.py
```

成果物は `build/vs18/plugins/Release/gstproresvk.dll`。`use-plugin.ps1` は現在のPowerShellだけにPATH、GST_PLUGIN_PATH、専用GST_REGISTRYを設定し、既存GStreamerのDLLを上書きしない。別パスはbuild/use-pluginの `-GStreamerRoot`、検査スクリプトの `--gst-root` で指定する。Windowsパスをgst-launchへ渡すときは `/` 区切りを推奨する。

再生表示を試す場合は末尾を `videoconvert ! d3d11videosink` に変更できる構成だが、今回の自動検査はfakesink/appsinkまで。表示の色や実時間の供給安定性は別途実測する。`rank=NONE` のため自動選択されず、`proresvkdec` を明示的に使う。

`device-index` はVulkanデバイスの番号（既定0）。`gpu-wait-timeout-ms` はフレームsemaphore待ち上限（既定10000ms）。どちらもNULL/READY時のみ変更する。後者はFFmpeg内部処理やドライバー呼び出し全体の時間を制限しない。検査スクリプトは各子プロセス全体にも90秒の上限を設ける。

## 時刻、色、所有権

- `GstVideoCodecFrame` を1対1で処理し、PTS/durationとsegment処理をGstVideoDecoderに保持させる。圧縮データのPTS/DTSはAVPacketへns単位で渡す。raw出力のDTSを圧縮入力のDTSと同一にする契約は設けない。
- ProResヘッダーの色情報を使い、未指定の原色・伝達特性・matrixだけ、入力capsに明記されたcolorimetryで補う。未指定を解像度から決めない。rangeはFFmpeg ProResのlimited出力を維持する。
- 既存合成HQ素材はMOVのcolrではBT.709だが、ProResヘッダーはprimaries=2、transfer=2（未指定）、matrix=1（BT.709）。今回のqtdemuxはProResの入力capsへcolrを渡さず、出力は `2:3:0:0` になる。原色・伝達特性はunknownとして保持する。ビットストリームとcapsそれぞれにBT.709を明示する追加検査も行う。
- 入力はpadding付きAVPacketにコピーする。GPU完了までAVFrameを保持し、CPU読み戻し後はGStreamer所有の独立バッファへコピーする。下流が出力を保持していても、デコーダー・Vulkanデバイスを破棄できる。
- FLUSH_STARTはGPU待ちループで確認する。FLUSH_STOP/seekではFFmpegの内部状態をflushする。EOSはdrainし、遅延出力を想定しない同期契約から外れた場合はエラーにする。

## 実機検査結果

再実行コマンドは `python scripts/test-plugin.py --out results/plugin-new`。記録は `results/plugin/summary.json` と同ディレクトリのコマンド別ログ。

| 検査 | 結果 |
|---|---|
| DLLロード、gst-inspect | 成功 |
| 1080p60 HQ / 4K60 HQ、各180フレームのパイプライン | EOSまで成功 |
| Proxy/LT/Standard/HQ、320×180・各30フレーム | 成功 |
| I422_10LE → videoconvert → BGRA | 成功 |
| 同じ要素で180フレーム → EOS → NULL → 再起動を3回 | PTS・フレーム数・durationあり・非黒Yを確認 |
| accurate flushing seek（1秒、0.5秒、2秒、0秒） | 各位置から3フレームのPTS一致 |
| 下流が保持した出力をstop/パイプライン破棄後に読む | SHA256一致 |
| 既知のBT.709をヘッダーまたはcapsに付与 | 出力の色情報一致 |
| 壊れたsignature、隠れたalpha/interlace、4444 caps | エラーとして終了 |
| 存在しないGPU番号、ファイル読み込み失敗 | エラーとして終了 |
| HQ先頭30フレームと単体FFmpeg Vulkan経路 | 全画素・SHA256完全一致 |

同じ30フレームのVulkan出力ハッシュは `e9294b6ecbc5c1e1540c36a2abce63d06eda6f825ea6f363e745fad79726a9a2`。これはGStreamerへの受け渡しによる画素の変化がないことの検査で、CPUデコードとの完全一致を意味しない。対応するCPU/Vulkan比較では10bit値の最大絶対差がY=26、U=17、V=27あった（`results/quality-hq-native-t1-plugin-precheck.json`）。原因・許容基準は未確定。

検査スクリプトのwall時間は起動・デバイス作成・破棄を含む疎通時間であり、最大fps・定常性能として扱わない。以前のD3D11共有テクスチャ経路の速度を、このsystem memory出力プラグインの性能として引用しない。

## 残課題と配布

最小要素としての動作は確認できた。次は実素材、長時間連続再生、起動・seekの反復、複数インスタンス、QoS/フレーム落ち、device lostを検査する。既知のFFmpeg初回停止がthread_count=1で根治したとは判断しない。GPUメモリ出力、純粋DX11版、表示込み性能、他GPUは後続工程。

実行時にこのDLLのほかFFmpeg SDKのavcodec-62/avutil-60とその依存DLL、GStreamer、Vulkan対応ドライバーが必要。今回のFFmpeg配布物はversion3有効のLGPL構成であり、配布時はFFmpeg本体と同梱依存物のライセンス・対応ソース・差替え可能性を確認する。GStreamerの同梱条件も別途確認する。独自コードの公開ライセンスはまだ選択しておらず、プラグインメタデータも `unknown`、originは予約ドメインの仮値としている。ローカル検証成果物であり、配布パッケージの完成を宣言しない。

参考：[GstVideoDecoder API](https://gstreamer.freedesktop.org/documentation/video/gstvideodecoder.html)、[FFmpeg ProRes Vulkan実装](https://github.com/FFmpeg/FFmpeg/blob/n8.1.3/libavcodec/vulkan_prores.c)。
