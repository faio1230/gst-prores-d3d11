# 独立したProRes GPUデコーダー

主実装は純粋DX11の `proresd3d11dec`。比較用にFFmpeg Vulkanの `proresvkdec` も保持する。限定対象で最終アーキテクチャは成立した。実素材1080p/4K、長時間、複数instance、表示／合成を検証済みだが、色の採用許容値、安定した実表示、他GPUを含む完成判定は[開発計画](プラグイン化計画.md)に従って継続する。

2026-09-24更新。GStreamer標準のパイプラインで使える `GstVideoDecoder` 派生の独立DLLとして検査した。環境はRTX 3070 / NVIDIA 591.86 / Windows 11 / GStreamer 1.28.2 MSVC x64。CPU画質参照にだけリポジトリ内固定FFmpeg 8.1.3を使う。最新の実素材・表示結果は[実素材と表示検証](実素材と表示検証.md)。

## 純粋DX11版：proresd3d11dec

```text
filesrc → qtdemux → proresd3d11dec → video/x-raw(memory:D3D11Memory)
                         │
                         ├ CPU: frame/picture/slice境界とjob記述を検査
                         ├ GPU: SM5 VLD、逆スキャン、逆量子化、逆DCT
                         ├ GPU: alphaなしはY/U/Vの3面R16_UNORM UAVへ直接書込み
                         └ GPU: alpha付きはalpha復号後にAYUV64へGPU内でpack
```

入力は`video/x-prores`の完全な1フレーム/バッファ。apco/apcs/apcn/apchの10bitとap4h/ap4xの12bitを、フレームヘッダーに応じた4:2:2または4:4:4、alphaなし/8/16bitで復号する。M4でTFF/BFFも採用し、RAWと範囲外のalpha modeは明示的に拒否する。CPU/Vulkanへのfallbackはない。対応表と検査結果は[機能対応表](機能対応表.md)、[アルファ拡張](アルファ拡張.md)、[インターレース拡張](インターレース拡張.md)。

受け取ったD3D11 deviceはDXGI adapterまでたどり、`DXGI_ADAPTER_FLAG_SOFTWARE`が立つWARP等をSM5対応でも拒否する。adapter情報を取得できない場合もGPU実行と推定せず拒否する。ソフトウェアflagがないことだけで物理GPUの動作保証とはしない。専用RGB要素にも同じ判定を適用する。WARPをGStreamer contextに注入したdecoderと、WARP製D3D11Memoryを専用RGB要素へ直接渡した実パイプラインは、どちらも`RESOURCE/FAILED`で停止した。他の物理GPUと実際のdevice lostは未検証。

alphaなしの出力は`video/x-raw(memory:D3D11Memory)`の`I422_10LE`、`Y444_10LE`、`I422_12LE`、`Y444_12LE`。Y/U/Vの3枚の`DXGI_FORMAT_R16_UNORM` texture（全てSRV|UAV）へ、IDCT shaderがpoolから受け取ったtextureに直接書く。alpha付きはGPUでalpha面を復号し、標準`AYUV64`の単一`R16G16B16A16_UNORM` textureへ全成分16bit UNORMとしてpackする。4面planarのD3D11プールはGStreamer 1.28.2で確保できないため、alphaはpacked画像の第1成分に保持する。M3採用時の独自capsフィールド`prores-depth`/`prores-chroma-shift`は廃止し、下流の標準`d3d11convert`と`videoconvert`の全画素互換性を確認した。通常経路の画像CPU読み戻しは0回。圧縮packetのCPU→GPU uploadと小さいVLD/alphaエラーフラグのGPU→CPU検査は行うため、処理全体を無条件に「ゼロコピー」とは呼ばない。

VLDエラーフラグのstaging readは、[Microsoftの`Map`仕様](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)に従い`D3D11_MAP_FLAG_DO_NOT_WAIT`で処理中を判定し、10秒のポーリング期限を設けた。各処理中応答で`GetDeviceRemovedReason()`も確認する。期限超過やHRESULT失敗は画像を出さずGStreamerエラーへ進む。期限・device lost・通常完了の分岐はGPUを失わせない単体試験で確認し、実GPUの復号/EOS/seekと公開素材779枚を回帰した。ただし実際のdevice lost、driver内部でAPI呼出し自体が停止する事態、全GPUでの期限保証は未検証。画像のCPU読み戻しは増やしていない。

```powershell
./scripts/build.ps1
. ./scripts/use-d3d11-plugin.ps1
gst-inspect-1.0 proresd3d11dec
gst-launch-1.0 -e filesrc location=media/synthetic-1080p60-hq.mov ! qtdemux ! proresd3d11dec ! fakesink sync=false
gst-launch-1.0 -e filesrc location=media/reference-proton-rec709-hq.mov ! qtdemux ! proresd3d11dec ! proresd3d11rgb ! d3d11videosink sync=true
./scripts/test-dx11.ps1
./scripts/test-d3d11-plugin.ps1
./scripts/test-d3d11-parser-mutations.ps1 -Iterations 500
```

成果物は `build/vs18/plugins/Release/gstproresd3d11.dll` と同じディレクトリの `prores_vld.cso`、`prores_idct_unorm.cso`、`prores_rgb.cso`、`prores_alpha.cso`、`prores_pack_alpha.cso`、`prores_rgb_alpha.cso`。Windows SDKのfxcでビルド時に `cs_5_0` へ固定し、起動時はbytecodeを読むだけにする。既定ではDLL隣を読み、`shader-directory` propertyまたは `PRORES_DX11_SHADER_DIR` で上書きできる。HLSL sourceも同じ出力先へライセンス確認用に配置する。`adapter=-1` は既定adapter。`rank=NONE` のため要素名を明示する。

DLLの直接依存はGStreamer D3D11とMSVC/Windows runtimeで、FFmpeg/Vulkan/D3DCompiler DLLはない。`use-d3d11-plugin.ps1` と `test-d3d11-plugin.ps1` はプロジェクトのFFmpegをPATHへ追加しない。D3D11 deviceはGStreamer context query/set_contextで共有し、poolの各memoryに作ったUAVをmemory寿命へ結び付けて再利用する。

2026-09-24に`scripts/stage-d3d11-plugin.ps1`で内部検証用の独立ステージを作成した。プラグインDLL、3つのCSOと対応HLSL、利用説明と主要検証文書だけを配置し、比較用Vulkan DLLを含めない。x64 PEと直接依存を`dumpbin`で検査し、ステージを作業ディレクトリにして、そのDLLから両要素を`gst-inspect`でロード。合成1080p/4Kの`I422_10LE(memory:D3D11Memory)`と、固定FFmpeg SDKで色タグを付けた合成1080pの`RGB10A2_LE(memory:D3D11Memory)`をfakesinkまで完走した。11ファイルのSHA256と依存DLL・検査結果は`results/stage-d3d11-internal-2026-09-24.json`。GStreamer SDKとMSVC runtimeは同梱しない。これは**内部検証ステージ**であり、公開ライセンス・対応ソースの提供方法、安定した実表示、他GPU/device lostが未達のため公開配布を認定しない。

同日の後続検査では`source/`へDX11版のC++/HLSL 8ファイルと専用CMake定義を同梱した。ステージ内のソースだけを入力として、GStreamer SDK・MSVC・Windows SDKでDLL/CSOを再ビルドし、再ビルドDLLから両要素をロードしてBT.709 RGB D3D11Memory経路を完走した。元DLL・再ビルドDLLとも直接依存にFFmpeg/Vulkan/D3DCompilerなし。cleanなコミット`1485eea`の20ファイルSHA256と検査結果は`results/stage-d3d11-source-rebuild-2026-09-24.json`。ソース同梱は内部での再現性を示すが、公開ライセンスの許諾表示・対応ソース提供条件や製品採用は未確定。

AC境界修正後のコミット`fdff052`でも、cleanな作業ツリーから21ファイルを内部ステージへ配置し、ステージ内ソースだけでDLL/CSOを再ビルドした。両要素のロード、合成1080p/4KのI422_10LEとBT.709 RGBのD3D11Memory出力が通過。記録は`results/stage-ac-boundary-2026-09-24.json`。公開配布の認定ではない。

M3採用後は`stage-d3d11-plugin.ps1`を6 CSOと対応ソースへ更新し、新しい内部ステージから独立再ビルドした。32ファイルのSHA256、禁止されたFFmpeg/Vulkan/D3DCompiler直接依存0、元DLLと再ビルドDLLの両方で要素ロードを確認。元DLLでI422_10LE、RGB10A2_LE、AYUV64、RGBA64_LEのD3D11Memoryパイプライン計5件を完走し、再ビルドDLLでもalpha付きAYUV64/RGBA64_LEを完走した。記録は`results/stage-d3d11-m3-internal-2026-09-24.json`。素材・表示・他GPU・公開ライセンスの未達が残るため、このステージも公開配布の認定ではない。

parserとVLD shaderのFFmpeg由来部分はSPDXでLGPL-2.1-or-laterを明記し、plugin metadataもLGPLとする。GStreamerはLGPL、fxc／D3D11はWindows SDKのビルド・実行依存。リポジトリ全体の独自コードの公開ライセンスは未決定なので、外部配布前にライセンス本文、著作権表示、対応ソースの提供方法を確定する。ProResの商標・特許・認証はOSSライセンスと別に確認する。

以下の表はM3以前のHQ中心の実機検証記録。現在のアルファ付き経路の採用結果は[アルファ拡張](アルファ拡張.md)を参照。

| 検査 | 結果 |
|---|---|
| 1080p/4K先頭フレーム全係数 | CPU参照と完全一致 |
| FFmpeg CPU画素との比較 | 両解像度で最大差1 |
| 実D3D11Memoryをdownloadした全4,147,200 sample | 最大差1、MAE 0.002699 |
| 1080p 180フレーム×3回、EOS→NULL→再起動 | フレーム数、PTS、duration、3面D3D11Memoryを確認 |
| accurate flushing seek（1秒、0.5秒、2秒、0秒） | 各位置から3フレームのPTS一致 |
| stop／pipeline破棄後に下流がbufferを保持 | 3面D3D11Memoryが有効 |
| header/capsの既知BT.709 | 出力colorimetry一致 |
| 同一pipelineで色のみBT.709→BT.601→BT.709 | 2回の再交渉で出力colorimetry・3面D3D11Memory・PTSが一致。最初と最後の画素も一致 |
| 正常なHQ出力後に隠れたalpha、または4444 capsへ切替 | alphaは`STREAM/FORMAT`、4444 capsは交渉エラーで停止。追加出力なし、先に出したD3D11Memoryはpipeline停止後も有効 |
| 外部contextのWARPをdecoderへ提供 | SM5対応でもDXGIソフトウェアadapterとして`RESOURCE/FAILED`。画像出力・CPU fallbackなし |
| WARP製I422 D3D11Memoryを専用RGB要素へ直接入力 | RGB出力前に`RESOURCE/FAILED`、追加GPU出力なし |
| BT.601出力をBT.709専用`proresd3d11rgb`へ入力 | サイレント変換せずエラー終了 |
| 破損signature、隠れたalpha/interlace、4444 caps、存在しないadapter/file | 全てエラー終了 |
| 構造は正常だが先頭DCが16bit範囲外のProRes frame | GPU VLDエラーフラグで`STREAM/DECODE`、画像を出さず停止。CPU参照も拒否 |
| 構造は正常だがAC runが係数面末尾の1つ先に達するframe | CPU参照は境界エラー、GPU VLDは同じjobを`STREAM/DECODE`で拒否。画像を出さず停止 |
| 定常D3D11Memory供給、6周×3試行中央値 | 1080p 293.8 fps、4K 133.1 fps |
| 検査用downloadでGPU完了を含む定常値 | 1080p 255.7 fps、4K 102.2 fps、p99 4.49/11.63 ms |
| 4K長時間、GPU完了込み | 30分、182,520フレーム、p99 11.42 ms、エラーなし |
| 2プロセス同時4K、GPU完了込み | 各91.08/91.86 fps、両方成功 |
| d3d11convert→RGB10A2 D3D11Memory→検査download | 1080p 152.6 fps、4K 68.0 fps、EOS成功 |
| 2入力decode→convert→d3d11compositor→検査download | 1080p 118.4 fps、EOS成功 |
| 公開カメラ由来1080p実写50フレーム | 固定FFmpeg CPU比Y/U/V最大差1 |
| 公開カメラ由来4K実写129フレーム | 固定FFmpeg CPU比Y/U/V最大差1 |
| RGB10A2全50フレーム、BT.709中央クロマCPU比 | R/G/B最大差5/2/5 |
| RGB10A2全129フレーム、BT.709中央クロマCPU比 | R/G/B最大差6/2/5 |
| clock同期の実表示1080p60／4K60 | 1080p 3試行drop 0、4K 3試行中1試行で14 drop |

VLDを1-bit反復loadから32-bit windowへ変更する前の検査用download込み保存記録は1080p 4.22 fpsだった。最終値はそのボトルネックを除き、shaderをビルド時CSOへ変更した後の値。D3D11Memory直結の初回bufferは1080p 229 ms、4K 232 msで、実行時compile版の約0.5〜0.9秒から改善した。同一プロセス内の起動100回はp95 31.08 ms、seek 1000回はp95 3.84 msで全て成功。同一deviceの2instanceも成功。他GPUは未測定。

D3D11下流検査ではdecoderの3面I422_10LEからd3d11convertのRGB10A2_LE、d3d11compositor出力まで `memory:D3D11Memory` を維持した。RGB10A2の全画素比較は実写1080p/4Kで行ったが、表示機器の色管理と素材のクロマ位置は未確定。compositor内部のrender/copy回数も未計測なので、この下流全体をゼロコピーとは呼ばない。保存ログは `results/proresd3d11-compositor-caps.log`。

`proresd3d11rgb`はalphaなしの3面D3D11Memoryを`prores_rgb.hlsl`で`RGB10A2_LE`へ、alpha付きの標準`AYUV64`を`prores_rgb_alpha.hlsl`で`RGBA64_LE`へCompute Shaderで直接書く。CPU読み戻しは検査経路だけで、通常経路にはない。入力はprogressive・limited BT.709に限定する。alphaなしの422では元のクロマ位置を中央補間し、444では各画素のクロマを使う。alpha付きAYUV64は既に4:4:4へ隣接複製した値を16bit UNORMとして扱い、元の422/444情報には依存しない。公開HQ実写1080p全50枚／4K全129枚／4K60全480枚とM2の18素材780枚で、alphaなしの独立BT.709式とのR/G/B最大差は各1 code。M3のalpha付き24条件48枚では、DX11が復号したAYUV値から独立に計算したRGB式との差最大1、alpha差0。EOS、seek、停止・破棄後のbuffer寿命も検査した。capsだけではSRV bind flagが保証されず、外部ソースの別構成は拒否し得る。表示機器の色管理、他GPUのtyped UAV対応、BT.709以外のRGBAは未達。詳細は[実素材と表示検証](実素材と表示検証.md)、[444・12bit拡張](444・12bit拡張.md)、[アルファ拡張](アルファ拡張.md)。

M5では標準colorimetryをBT.601/709/2020、PQ、HLGの5条件で確認し、318×178の422と319×179の444、同一pipelineでの色・解像度・形式変更をCPU全画素と照合した。奇数幅422は拒否する。専用RGB要素はBT.709限定を維持し、319×179の444も独立式で最大差1 code。詳細は[M5検証](M5色タグ・端数寸法・形式変更.md)。

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

出力は `video/x-raw,format=I422_10LE`、system memory。10bitのY/U/Vを16bit little endian容器で保持する。色変換・8bit化・スケーリングは行わない。MOVの読み込みと表示は別のGStreamer要素で行う。

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
