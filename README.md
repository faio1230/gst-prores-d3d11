# Windows向けGStreamer ProRes GPUデコーダー

**最終目標は、純粋なDirect3D 11で通常のProResをGPUデコードし、復号画像をGPUメモリのままGStreamerへ出力する独立プラグインです。** DX11デコードとGPUメモリ出力の両方を必須の完成条件とします。利用先アプリのコードは変更せず、LTC・プレイリスト・Spout・管理UIは含めません。

完成形は `video/x-prores → DX11 Compute Shaderによる復号 → video/x-raw(memory:D3D11Memory)`。復号画像をCPUへ読み戻さず、Vulkanに依存しない経路を目指します。現在の `proresvkdec` はFFmpeg Vulkanを使う比較・検証用の中間成果で、最終成果物ではありません。以後の主実装はDX11ネイティブ復号とD3D11Memory出力に置きます。

**progressive ProRes 422 HQ / 10bitに限定した最初の純粋DX11経路が動作していますが、採用完了ではありません。** `proresd3d11dec` はCPUで境界を検査し、SM5でVLD・逆スキャン・逆量子化・逆DCTを行い、GStreamerの3面D3D11Memoryへ直接出力します。通常経路に画像のCPU復号・読み戻し、Vulkan、libavcodec、画像のGPU内コピーはありません。RTX 3070で1080p/4Kの全係数がCPU参照と完全一致し、FFmpeg CPU画素との差は最大1でした。EOS、seek、再起動、バッファ寿命も検査済みです。実素材、長時間・複数instance、表示／合成、他GPUは未検証なのでgoal全体は継続します。

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
./scripts/generate-media.ps1
. ./scripts/use-plugin.ps1
gst-launch-1.0 -e filesrc location=media/synthetic-1080p60-hq.mov ! qtdemux ! proresd3d11dec ! fakesink sync=false
```

GStreamer MSVC x64ランタイムと開発SDKが必要です。[プラグインの実行手順・対応範囲](docs/GStreamerプラグイン.md)を参照してください。比較用の `proresvkdec` はVulkan復号後にCPUへ読み戻しますが、最終経路の `proresd3d11dec` はD3D11Memoryを出力します。

合成素材180フレームの起動・終了込みwall throughputはD3D11Memory直結で1080pが139.1 fps、4Kが88.2 fpsでした。正式な定常性能や実運用性能ではなく、CPU/GPU負荷、p99、複数入力、実素材、表示を含む測定は残っています。Vulkan経路の既知の画素差・初回停止とDX11経路の結果は分けて扱います。

- [調査・方式比較](docs/調査と方式比較.md)
- [ビルド・実行](docs/ビルドと実行.md)
- [測定方法・制限](docs/測定仕様.md)
- [測定結果](docs/測定結果.md)
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
