# Windows向けGStreamer ProRes GPUデコーダー

通常のProResをFFmpegのVulkanデコーダーで処理する、独立したGStreamerプラグイン `proresvkdec` と検証プログラムです。利用先アプリのコードは変更していません。LTC・プレイリスト・Spout・管理UIは含めません。

**実験版プラグインが動作しています。製品採用は未判定です。** 初版はprogressive ProRes 422（Proxy/LT/Standard/HQ）をGPUで復号し、system memoryの `I422_10LE` を出力します。1080p/4K、EOS、再起動、seek、色メタデータ、バッファ寿命を実機検査しました。4444・アルファ・インターレース・ProRes RAWは初版対象外です。

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
./scripts/generate-media.ps1
. ./scripts/use-plugin.ps1
gst-launch-1.0 -e filesrc location=media/synthetic-1080p60-hq.mov ! qtdemux ! proresvkdec ! fakesink sync=false
```

GStreamer MSVC x64ランタイムと開発SDKが必要です。[プラグインの実行手順・対応範囲](docs/GStreamerプラグイン.md)を参照してください。CPUへの読み戻しを含み、ゼロコピー出力ではありません。純粋なDX11デコーダーはまだ未実装です。

既存の単体検証では4KのGPU内転送・オフスクリーン合成に改善が見られましたが、その高速D3D11経路は今回のプラグインへまだ接続していません。CPU/GPU画素差、初回停止の既知事象、実素材未評価も残っています。

- [調査・方式比較](docs/調査と方式比較.md)
- [ビルド・実行](docs/ビルドと実行.md)
- [測定方法・制限](docs/測定仕様.md)
- [測定結果](docs/測定結果.md)
- [DX11ネイティブ実装の検証](docs/DX11実装検証.md)
- [画素不一致・停止の再現情報](docs/不一致の再現.md)
- [GStreamerプラグイン化計画](docs/プラグイン化計画.md)
- [実験版プラグインと検査結果](docs/GStreamerプラグイン.md)

## 実装済み

`prores_bench` は同一FFmpeg SDKでCPU/Vulkanを切り替え、GPU完了待ち、CPU読み戻し、D3D11アップロード、Win32共有テクスチャ、複数層の検証合成を比較します。フレームごとの時刻・所要時間をCSV、初期化・定常速度・CPU・メモリをJSONへ記録します。

`dx11_primitives` は **D3D11 Compute Shaderによるビット読み取りと8×8逆DCTの実機検証**です。通常ProResの完全なDX11デコーダーではありません。移植に必要な処理がSM5で実行可能なことと、残る実装範囲を区別します。

## 最短の実行

Windows x64、MSVC C++ビルドツール、Windows SDK、CMake、Pythonを使用します。PowerShell 7でリポジトリのルートから実行します。

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
./scripts/generate-media.ps1
python scripts/benchmark.py media/synthetic-2160p60-hq.mov --modes cpu-d3d11 interop --layers 4 --loops 12 --repeats 3 --out results/my-run
./build/vs18/Release/dx11_primitives.exe
```

依存物は `tools/` と `external/`、合成素材は `media/` に置き、Gitには含めません。FFmpeg、GStreamer、PATH、ドライバーのシステム設定は変更しません。実機はRTX 3070一台のみで、他GPU・ドライバーの動作は未確認です。
