# Windows向けGStreamer ProRes GPUデコーダー

**最終目標は、外部アプリケーションから独立したGStreamer用ProRes GPUデコーダーです。** 純粋なDirect3D 11 Compute ShaderでProResを復号し、復号画像をCPUへ読み戻さず `GstD3D11Memory` のまま出力することを、両立必須の完成条件とします。専用の動画デコード回路を使う方式ではありません。このリポジトリの要件、採用判定、配布物はGStreamerプラグインとその検証に限定し、外部アプリ固有の同期・再生制御・UI・入出力機能を前提にしません。

完成形は `video/x-prores → DX11 Compute Shaderによる復号 → video/x-raw(memory:D3D11Memory)`。復号画像をCPUへ読み戻さず、Vulkanに依存しない経路を目指します。現在の `proresvkdec` はFFmpeg Vulkanを使う比較・検証用の中間成果で、最終成果物ではありません。以後の主実装はDX11ネイティブ復号とD3D11Memory出力に置きます。

**progressive ProRes 422 HQ / 10bitに限定した最初の純粋DX11経路が動作していますが、採用完了ではありません。** `proresd3d11dec` はCPUで境界を検査し、SM5でVLD・逆スキャン・逆量子化・逆DCTを行い、GStreamerの3面D3D11Memoryへ直接出力します。通常経路に画像のCPU復号・読み戻し、Vulkan、libavcodec、画像のGPU内コピーはありません。RTX 3070で1080p/4Kの全係数がCPU参照と完全一致し、FFmpeg CPU画素との差は最大1でした。EOS、seek、再起動、バッファ寿命、4K 30分、複数instance、D3D11変換・合成も検査済みです。公開カメラ由来1080p実写50フレーム、4K実写129フレーム、4K60実写480フレームの全画素比較も最大差1で通過しましたが、4K実表示にはdropとOS未表示が残り、RGBの採用許容値と他GPUも未確定です。

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
./scripts/generate-media.ps1
. ./scripts/use-d3d11-plugin.ps1
gst-launch-1.0 -e filesrc location=media/synthetic-1080p60-hq.mov ! qtdemux ! proresd3d11dec ! fakesink sync=false
```

GStreamer MSVC x64ランタイムと開発SDKが必要です。[プラグインの実行手順・対応範囲](docs/GStreamerプラグイン.md)を参照してください。比較用の `proresvkdec` はVulkan復号後にCPUへ読み戻しますが、最終経路の `proresd3d11dec` はD3D11Memoryを出力します。

6周×3試行の定常D3D11Memory供給は1080p 293.8 fps、4K 133.1 fps。検査用downloadでGPU完了まで含めても255.7/102.2 fps、p99は4.49/11.63 msでした。4Kの30分・182,520フレーム、起動100回、seek 1000回、2同時decodeに加え、RGB10A2 D3D11Memoryへの変換と2入力d3d11compositorも成功しています。実写1080pとclock同期の実表示まで試した結果・未達項目は[実素材と表示検証](docs/実素材と表示検証.md)に記録しました。Vulkan経路の既知の画素差・初回停止とDX11経路の結果は分けて扱います。

- [調査・方式比較](docs/調査と方式比較.md)
- [ビルド・実行](docs/ビルドと実行.md)
- [測定方法・制限](docs/測定仕様.md)
- [測定結果](docs/測定結果.md)
- [実素材・色・実表示の検証](docs/実素材と表示検証.md)
- [DX11ネイティブ実装の検証](docs/DX11実装検証.md)
- [画素不一致・停止の再現情報](docs/不一致の再現.md)
- [GStreamerプラグイン化計画](docs/プラグイン化計画.md)
- [純粋DX11実装の進捗](docs/進捗.md)
- [GStreamerプラグインと検査結果](docs/GStreamerプラグイン.md)

## 実装済み

`prores_bench` は同一FFmpeg SDKでCPU/Vulkanを切り替え、GPU完了待ち、CPU読み戻し、D3D11アップロード、Win32共有テクスチャ、複数層の検証合成を比較します。フレームごとの時刻・所要時間をCSV、初期化・定常速度・CPU・メモリをJSONへ記録します。

`dx11_primitives` はD3D11 Compute Shaderによるビット読み取りと8×8逆DCTの基礎検証です。`prores_dx11_coeff` は固定SDKでMOVから実パケットを取り出し、独立CPU参照とDX11の全係数・画素を比較します。`proresd3d11dec` は同じparser/shaderをGstVideoDecoderとして連続実行し、`I422_10LE(memory:D3D11Memory)` を出力します。検査用CPU読み戻しは通常経路と分離しています。

## 最短の実行

Windows x64、MSVC C++ビルドツール、Windows SDK、CMake、Pythonを使用します。PowerShell 7でリポジトリのルートから実行します。

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
./scripts/generate-media.ps1
python scripts/benchmark.py media/synthetic-2160p60-hq.mov --modes cpu-d3d11 interop --layers 4 --loops 12 --repeats 3 --out results/my-run
./build/vs18/Release/dx11_primitives.exe
./scripts/test-dx11.ps1
./scripts/test-d3d11-plugin.ps1
```

依存物は `tools/` と `external/`、合成素材は `media/` に置き、Gitには含めません。FFmpeg、GStreamer、PATH、ドライバーのシステム設定は変更しません。実機はRTX 3070一台のみで、他GPU・ドライバーの動作は未確認です。
