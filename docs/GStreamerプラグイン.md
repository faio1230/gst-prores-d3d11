# 独立したProRes GPUデコーダー

主実装は純粋DX11の `proresd3d11dec`。比較用にFFmpeg Vulkanの `proresvkdec` も保持する。限定対象で最終アーキテクチャは成立したが、実素材、長時間、複数instance、表示／合成、他GPUを含む完成判定は[開発計画](プラグイン化計画.md)に従って継続する。

2026-09-22。利用先アプリを外し、GStreamer標準のパイプラインで使える `GstVideoDecoder` 派生の独立DLLとして検査した。環境はRTX 3070 / NVIDIA 591.86 / Windows 11 / GStreamer 1.28.2 MSVC x64。CPU画質参照にだけリポジトリ内固定FFmpeg 8.1.3を使う。

## 純粋DX11版：proresd3d11dec

```text
filesrc → qtdemux → proresd3d11dec → video/x-raw(memory:D3D11Memory)
                         │
                         ├ CPU: frame/picture/slice境界とjob記述を検査
                         ├ GPU: SM5 VLD、逆スキャン、逆量子化、逆DCT
                         └ GPU: Y/U/Vの3面R16_UNORM UAVへ直接書込み
```

入力は `video/x-prores,variant=hq` の完全な1フレーム/バッファで、progressive、4:2:2、10bit、alphaなしに限定する。Proxy/LT/Standard、4444/XQ、12bit、alpha、interlaced、RAWは明示的に対象外。CPU/Vulkanへのfallbackはない。

出力は `video/x-raw(memory:D3D11Memory),format=I422_10LE`。Yは幅×高さ、U/Vは幅/2×高さの3枚 `DXGI_FORMAT_R16_UNORM` textureで、全てSRV|UAV。IDCT shaderがpoolのtextureへ直接書くため通常経路の画像GPU copyは0回、画像のCPU読み戻しも0回。圧縮packetのCPU→GPU uploadと、小さいVLDエラーフラグのGPU→CPU検査は行うため、処理全体を無条件に「ゼロコピー」とは呼ばない。

```powershell
./scripts/build.ps1
. ./scripts/use-plugin.ps1
gst-inspect-1.0 proresd3d11dec
gst-launch-1.0 -e filesrc location=media/synthetic-1080p60-hq.mov ! qtdemux ! proresd3d11dec ! fakesink sync=false
./scripts/test-dx11.ps1
./scripts/test-d3d11-plugin.ps1
```

成果物は `build/vs18/plugins/Release/gstproresd3d11.dll` と同じディレクトリの `prores_vld.hlsl`、`prores_idct.hlsl`。shaderは実行時に `cs_5_0` へコンパイルする。既定ではDLL隣を読み、`shader-directory` propertyまたは `PRORES_DX11_SHADER_DIR` で上書きできる。`adapter=-1` は既定adapter。`rank=NONE` のため要素名を明示する。

DLLの直接依存はGStreamer D3D11、D3DCompiler、MSVC/Windows runtimeで、FFmpeg/Vulkan DLLはない。`test-d3d11-plugin.ps1` はプロジェクトのFFmpegをPATHへ追加せず検査する。D3D11 deviceはGStreamer context query/set_contextで共有し、poolの各memoryに作ったUAVをmemory寿命へ結び付けて再利用する。

実機結果は次のとおり。

| 検査 | 結果 |
|---|---|
| 1080p/4K先頭フレーム全係数 | CPU参照と完全一致 |
| FFmpeg CPU画素との比較 | 両解像度で最大差1 |
| 実D3D11Memoryをdownloadした全4,147,200 sample | 最大差1、MAE 0.002699 |
| 1080p 180フレーム×3回、EOS→NULL→再起動 | フレーム数、PTS、duration、3面D3D11Memoryを確認 |
| accurate flushing seek（1秒、0.5秒、2秒、0秒） | 各位置から3フレームのPTS一致 |
| stop／pipeline破棄後に下流がbufferを保持 | 3面D3D11Memoryが有効 |
| header/capsの既知BT.709 | 出力colorimetry一致 |
| 破損signature、隠れたalpha/interlace、4444 caps、存在しないadapter/file | 全てエラー終了 |
| 180フレームwall throughput、D3D11Memory直結 | 1080p 139.1 fps、4K 88.2 fps |

wall throughputはshader compile、起動、demux、終了を含む単回の疎通値で、正式な定常性能ではない。VLDを1-bit反復loadから32-bit windowへ変更する前の検査用download込み保存記録は1080p 4.22 fpsであり、現在値はそのボトルネックを除いた後の結果。p99、CPU/GPU負荷、30分以上、起動100回、seek1000回、複数instance、実際のD3D11合成・表示は未測定。

## 比較用Vulkan版：proresvkdec

## Vulkan版の構成

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

4444/4444 XQ、アルファ、12bit、インターレース、RAWは対象外。既存の4444検証で輝度異常を確認したため、この比較版で対応を宣言しない。純粋DX11版の結果とは分けて扱う。

## Vulkan版のビルドと実行

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

## Vulkan版の時刻、色、所有権

- `GstVideoCodecFrame` を1対1で処理し、PTS/durationとsegment処理をGstVideoDecoderに保持させる。圧縮データのPTS/DTSはAVPacketへns単位で渡す。raw出力のDTSを圧縮入力のDTSと同一にする契約は設けない。
- ProResヘッダーの色情報を使い、未指定の原色・伝達特性・matrixだけ、入力capsに明記されたcolorimetryで補う。未指定を解像度から決めない。rangeはFFmpeg ProResのlimited出力を維持する。
- 既存合成HQ素材はMOVのcolrではBT.709だが、ProResヘッダーはprimaries=2、transfer=2（未指定）、matrix=1（BT.709）。今回のqtdemuxはProResの入力capsへcolrを渡さず、出力は `2:3:0:0` になる。原色・伝達特性はunknownとして保持する。ビットストリームとcapsそれぞれにBT.709を明示する追加検査も行う。
- 入力はpadding付きAVPacketにコピーする。GPU完了までAVFrameを保持し、CPU読み戻し後はGStreamer所有の独立バッファへコピーする。下流が出力を保持していても、デコーダー・Vulkanデバイスを破棄できる。
- FLUSH_STARTはGPU待ちループで確認する。FLUSH_STOP/seekではFFmpegの内部状態をflushする。EOSはdrainし、遅延出力を想定しない同期契約から外れた場合はエラーにする。

## Vulkan版の実機検査結果

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

## Vulkan版の残課題と配布

比較用の最小要素としての動作は確認できた。実素材、長時間連続再生、起動・seekの反復、複数インスタンス、QoS/フレーム落ち、device lostは未検査。既知のFFmpeg初回停止がthread_count=1で根治したとは判断しない。最終実装の判定は上記DX11版に対して行う。

実行時にこのDLLのほかFFmpeg SDKのavcodec-62/avutil-60とその依存DLL、GStreamer、Vulkan対応ドライバーが必要。今回のFFmpeg配布物はversion3有効のLGPL構成であり、配布時はFFmpeg本体と同梱依存物のライセンス・対応ソース・差替え可能性を確認する。GStreamerの同梱条件も別途確認する。独自コードの公開ライセンスはまだ選択しておらず、プラグインメタデータも `unknown`、originは予約ドメインの仮値としている。ローカル検証成果物であり、配布パッケージの完成を宣言しない。

参考：[GstVideoDecoder API](https://gstreamer.freedesktop.org/documentation/video/gstvideodecoder.html)、[FFmpeg ProRes Vulkan実装](https://github.com/FFmpeg/FFmpeg/blob/n8.1.3/libavcodec/vulkan_prores.c)。
