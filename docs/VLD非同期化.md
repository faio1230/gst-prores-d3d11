# VLDエラー検査の非同期化

## 変更した設計

`proresd3d11dec`はProRes 422 HQのVLDをDX11 Compute Shaderで実行し、CPUによる毎フレームのエラーフラグMap完了待ちを廃止した。GPU命令はVLD→IDCT→エラーフラグのstagingコピーの順に投入し、画像は従来どおり3面の`GstD3D11Memory`で返す。エラーフラグのstagingを3面のリングとし、4枚目の投入前に最古の結果を回収する。EOS/drainでは残りをすべて回収する。破損が見つかれば`STREAM/DECODE`を通知して以後の復号を停止する。flushing seekでは旧segmentの未回収結果を破棄し、新segmentへエラーを持ち込まない。

IDCT shader側でエラーを読んで出力を無効化する案は選ばなかった。VLDのjob別エラーを各IDCT blockへ対応させる追加のGPU依存と、異常画素をdownstreamへ見せないための別の同期・出力管理が必要になる。3面リングなら既存のVLD shaderと画素演算を変えず、単一フレームの破損もEOS回収で確実にエラーへできる。代わりに破損フレームの画像がエラー通知より先にdownstreamへ渡る可能性があり、通知は通常最大3フレーム遅れる。フラッシュで破棄した旧segmentの未回収GPUエラーは報告しない。

リング再利用とEOSの回収時だけ、`Flush()`で命令を提出した後に`Map(DO_NOT_WAIT)`を繰り返す。各MapとUnmapの間だけ`GstD3D11Device` lockを取り、反復の合間は解放する。10秒期限と`GetDeviceRemovedReason()`検査は残した。診断用timestamp queryの待機もlockを解放して行う。GStreamer 1.28.2の[標準sink実装](https://github.com/GStreamer/gstreamer/blob/1.28.2/subprojects/gst-plugins-bad/sys/d3d11/gstd3d11videosink.cpp#L1621-L1646)は`gst_d3d11_window_render`を呼び、その[window実装](https://github.com/GStreamer/gstreamer/blob/1.28.2/subprojects/gst-plugins-bad/sys/d3d11/gstd3d11window.cpp#L986-L995)が同じdevice lockを取得する。従来のMap待機中はsinkのレンダリングと競合し得たが、今回の回収待ちはlockを保持し続けない。

## 検証と採用判定

RTX 3070、固定GStreamer 1.28.2でビルド、係数完全一致、合成1080p/4Kの画素差最大1 code、EOS×3・flushing seek×4・RGB・動的caps・寿命試験を通過。公開実素材50+129+480+120=779枚はD3D11MemoryのままEOSまで完走した。固定SDKで抽出したpacketのASan付き決定的変異2,000件を通過し、AC境界単一byte再現例とDC異常をGPUでも`STREAM/DECODE`で拒否した。EOSを送らず破損1枚目を4枚目のリング再利用で検出するテストも追加した。通常QoS・Present(0)の実写4K60×10周パイロットは、VLD→コピー→IDCT版とVLD→IDCT→コピー版の両方で4800/4800枚だった。

パイロット2試行は交互4組の採用ゲートではない。旧版`a3b4315`と新版の独立ステージで、通常QoSの10周×交互4組、回収待ちとdecode全体のp99・最大値、1080p/4K direct性能の中央値差を確認してから採否を決める。OS/DWM未表示はdecoder採用ゲートに含めず、必要なら参考値として分けて記録する。
