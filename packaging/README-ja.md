# ProRes DX11 GStreamerプラグイン（内部検証用）

このディレクトリは独立DLLの動作検証用ステージです。公開配布物としては未完成です。リポジトリの公開ライセンス、派生ソースの提供方法、実表示と他GPUの採用判定が未完了です。

`gstproresd3d11.dll` は `proresd3d11dec`（ProRes復号）と `proresd3d11rgb`（BT.709専用RGB変換）を登録します。復号器の対応範囲は progressive ProRes 422 HQ、10bit、alphaなし。Proxy/LT/Standard、4444/XQ、interlace、12bit、alphaは対象外です。CPU/Vulkanへの暗黙fallbackはありません。

復号出力は `video/x-raw(memory:D3D11Memory),format=I422_10LE` で、3枚のR16_UNORM textureです。通常経路で復号画像をCPUへ読み戻しません。RGB要素はBT.709 limited、中央または未指定のクロマ位置に限り、`RGB10A2_LE(memory:D3D11Memory)` を出力します。

検証済み環境はWindows x64、GStreamer 1.28.2 MSVC x64、Direct3D feature level 11_0以上・R16_UNORM typed UAV対応のRTX 3070です。DXGIでソフトウェアと報告されるWARP等のdeviceは、SM5対応でもGPU復号器として受け付けません。他の物理GPUの動作は未検証です。GStreamer本体とMSVC runtimeは同梱していません。プラグインDLLと3つの`.cso`を同じディレクトリに置いてください。

PowerShellでGStreamerの`bin`をPATH先頭に置き、`GST_PLUGIN_PATH`をこのディレクトリへ設定してから、`gst-inspect-1.0 proresd3d11dec` と `gst-inspect-1.0 proresd3d11rgb` で登録を確認できます。独立したパイプライン例：

```text
filesrc location=sample.mov ! qtdemux ! proresd3d11dec ! video/x-raw(memory:D3D11Memory),format=I422_10LE ! fakesink sync=false
```

素材の色がBT.709と確認できる場合に限るRGB例：

```text
filesrc location=bt709-hq.mov ! qtdemux ! proresd3d11dec ! proresd3d11rgb ! video/x-raw(memory:D3D11Memory),format=RGB10A2_LE ! fakesink sync=false
```

`source/`にはDX11版DLLのC++/HLSLソースと、FFmpeg/Vulkan SDKを必要としない単独ビルド定義を含めます。Visual StudioのMSVC x64・Windows SDK・GStreamer MSVC x64 SDKを用意し、ステージのディレクトリから次のように再ビルドできます（generator名はインストール済みのVisual Studioに合わせてください）。

```powershell
cmake -S source -B source-build -G "Visual Studio 18 2026" -A x64 "-DGSTREAMER_ROOT=C:/Program Files/gstreamer/1.0/msvc_x86_64"
cmake --build source-build --config Release
```

このソース同梱は内部での再現性検査であり、公開ライセンスの許諾表示や対応ソース提供条件を確定したことを意味しません。`manifest.json`はファイルSHA256、DLLの直接依存、分離したディレクトリからの動作とソース再ビルドの検査を記録します。画質・EOS・seek・実表示の正式な証拠と未達事項は、同梱の`docs/採用判断サマリー.md`と`docs/GStreamerプラグイン.md`、リポジトリの`docs/進捗.md`を参照してください。
