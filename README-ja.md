# gst-prores-d3d11

GStreamerで単独利用できる、Direct3D 11 Compute Shader製のProResデコーダーです。復号画像をCPUへ読み戻さず`GstD3D11Memory`で出力します。CPU、Vulkan、FFmpegによる復号へのfallbackはありません。CPUは圧縮packetの検査と準備を行い、画像の復号処理はGPUで実行します。GPU専用の固定機能ProResデコーダーを使う方式ではありません。

## 対応する入力・出力

| 項目 | 対応範囲 |
| --- | --- |
| プロファイル | Proxy（`apco`）、LT（`apcs`）、Standard（`apcn`）、HQ（`apch`）、4444（`ap4h`）、4444 XQ（`ap4x`）。クロマとアルファはFourCC名だけでなくpicture headerで決まります。 |
| クロマ・深度 | 4:2:2／4:4:4。`apco`～`apch`は10bit、`ap4h`／`ap4x`は12bit。アルファなしの出力は`I422_10LE`、`Y444_10LE`、`I422_12LE`、`Y444_12LE`。 |
| アルファ | なし／8bit／16bit。アルファ付き出力は標準の16bit`AYUV64`。4:2:2のクロマは隣接画素へ複製します。 |
| フィールド | progressive、TFF、BFF。デインターレースは下流で行います。 |
| 色 | BT.601／BT.709／BT.2020／PQ／HLGのタグを標準出力capsへ伝えます。`proresd3d11rgb`の入力はprogressive・limited BT.709のみです。 |
| 寸法・途中変更 | 16の倍数でない寸法に対応し、4:4:4は奇数幅も可。4:2:2は偶数幅が必要です。色・寸法・出力形式の途中変更はcaps再交渉で扱います。 |

検証した組合せと正確な制限は[機能対応表](docs/機能対応表.md)を参照してください。

## 要件

Windows x64、GStreamer 1.28以降（実機検証は1.28.2）、feature level 11_0以上の物理Direct3D 11 GPU、PowerShell 7、MSVC C++ Build Tools、`fxc.exe`を含むWindows SDK、CMake、Pythonが必要です。GPU検証はNVIDIA RTX 3070のみで、WARPなどのソフトウェアadapterは拒否します。ビルドスクリプトは検査器のため固定FFmpeg SDKを使いますが、デコーダープラグイン自体はFFmpegへリンクしません。

## ビルドと実行

リポジトリのルートでPowerShell 7から実行します。

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
. ./scripts/use-d3d11-plugin.ps1
```

`use-d3d11-plugin.ps1`が設定するのは現在のシェルだけです。GStreamerの配置が既定のMSVC x64パスと異なる場合は、ビルドと設定の両スクリプトへ`-GStreamerRoot`を渡します。次の`sample.mov`は手元のProRes MOVへ置き換えてください。

```powershell
# 表示変換せずD3D11Memoryへ復号。
gst-launch-1.0 -e filesrc location=sample.mov ! qtdemux ! proresd3d11dec ! "video/x-raw(memory:D3D11Memory)" ! fakesink sync=false

# progressive・limited BT.709をGPUでRGB変換して表示。
gst-launch-1.0 -e filesrc location=sample.mov ! qtdemux ! proresd3d11dec ! proresd3d11rgb ! d3d11videosink

# アルファ付き素材を標準AYUV64のままsinkへ渡す。
gst-launch-1.0 -e filesrc location=sample.mov ! qtdemux ! proresd3d11dec ! "video/x-raw(memory:D3D11Memory),format=AYUV64" ! d3d11videosink
```

詳しくは[ビルドと実行](docs/ビルドと実行.md)、[設計](docs/設計.md)、[検証](docs/検証.md)を参照してください。

## 精度と性能

検証素材のYUV全画素は、固定FFmpeg SDKのCPU復号結果と元の深度で最大1 codeの差でした。アルファは比較に使った出力深度で一致しています。専用RGB要素も独立したBT.709式との差がRGB10A2で最大1 codeでした。RTX 3070での合成HQ素材のdirect出力は、1080p約**436 fps**、4K約**258 fps**です。GPU完了待ちと表示時間を含めない復号器の供給速度であり、再生速度の保証ではありません。

## 既知の制限

- AMD／Intel GPU、実際のdevice lostと復旧、他環境での長時間動作は未検証です。
- HDR色タグは伝播しますが、HDR→RGBの数値精度は未検証です。専用RGB要素はprogressive・limited BT.709に限定されます。
- 奇数幅の4:2:2は拒否します。`proresd3d11rgb`はデインターレースしないため、インターレースのRGB表示には下流の別要素が必要です。
- GPUエラーを非同期で検査するため、破損bitstreamのエラー通知が最大3フレーム遅れ、該当画像が先に下流へ渡る可能性があります。
- decoder QoSの測定はOS/DWMによる実表示や表示装置の色精度を証明しません。再配布条件が明確なカメラ由来のアルファ／インターレース実素材も未検証です。

## ライセンスと商標

ライセンスはLGPL-2.1-or-laterです。parserとVLD shaderには、FFmpegの`proresdec.c`（および関連するProRes VLD実装）に由来する部分があり、該当ソースにはSPDX表示があります。ProRes is a trademark of Apple Inc. This project is not affiliated with Apple.
