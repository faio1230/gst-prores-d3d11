# OSS公開前のDX11境界修正

2026-09-25。対象は純粋DX11の`proresd3d11dec`と、そのD3D11Memoryを読む`proresd3d11rgb`。CPU参照はリポジトリ内固定SDK `tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe`を使う。長い実行ログ・中間rawは`build/oss-prepublish/`へ置き、コミットする判定値だけを`results/`に保存する。

## Dispatch上限

[D3D11のDispatch仕様](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-dispatch)は各軸65535グループ以下。旧IDCTは1ブロック＝1グループでX軸に全ブロックを並べ、4K 4:2:2で259,200、1080p 4:4:4で97,920となり上限外だった。新実装は`X=min(ブロック数,65535)`、`Y=ceil(ブロック数/65535)`とし、shader側で`index=SV_GroupID.y*65535+SV_GroupID.x`へ戻す。端の余剰グループは`block_count`で除外する。検査器のIDCT Dispatchも同じ式にした。

| 条件 | IDCTグループ | Dispatch X×Y |
|---|---:|---:|
| 1080p 4:4:4 | 97,920 | 65,535×2 |
| 4K 4:2:2 | 259,200 | 65,535×4 |
| 4K 4:4:4＋alpha | 388,800 | 65,535×6 |
| 8192×8192 4:4:4＋alpha | 3,145,728 | 65,535×49 |

8192²ではマクロブロック512²＝262,144個、最小スライス幅1ブロックなら最大スライス数も262,144。各スライスのVLDは3ジョブなので`ceil(3×262144/64)=12288`グループ、alphaは`ceil(262144/64)=4096`グループ。packとRGBは8×8スレッドなので各軸`ceil(8192/8)=1024`グループ。いずれも各軸65535以下。この算術をスモークのstatic assertionで固定した。これはDispatch範囲の検査であり、8192²フレームのGPUメモリ確保・連続処理の保証ではない。

この環境では`C:\Windows\System32\d3d11sdklayers.dll`がなく、任意条件のdebug layer検査は実施できなかった。4K 4:2:2と4K 4:4:4＋alphaの通常deviceでの復号・EOSは別に実行したため、debug layerのエラー0とは記さない。

## matrix=0／2とクロマ位置

固定SDKと同系の[FFmpeg n8.1 `proresdec.c`](https://ffmpeg.org/doxygen/8.1/proresdec_8c_source.html#l00282)はヘッダbyte 16を`colorspace`へそのまま写す。[FFmpegの列挙値](https://ffmpeg.org/doxygen/8.1/pixfmt_8h_source.html#l00701)では0はRGB、2は未指定であり、FFmpegが0を未指定へ変換しているわけではない。本デコーダーはYUVテクスチャを出すため、出力matrix=RGBは不正。0と2をGStreamer側ではUNKNOWNとし、RGB以外の上流caps matrixがあればそこから補う。スモークでは両値を注入し、BT.709／BT.601へのフォールバックと画素不変を検査する。

[Apple ProRes白書の4:2:2配置図](https://www.apple.com/tw/final-cut-pro/docs/Apple_ProRes_White_Paper.pdf#page=8)、[GStreamerの水平co-sited `mpeg2` chroma-site](https://gstreamer.freedesktop.org/documentation/video/gstvideochroma.html)に合わせ、planar 4:2:2の出力capsを`chroma-site=mpeg2`とし、専用RGB shaderはクロマ位置`x/2`で左右を線形補間する。以前の`(x−0.5)/2`は中央配置だった。専用RGB要素は4:2:2の位置タグがない／`mpeg2`以外の入力を拒否する。独立BT.709式も同じ左寄せへ変更する。アルファ付きAYUV64は既存どおり隣接2画素へクロマを複製した4:4:4なので、この補間変更の対象外。

ただしFFmpeg n8.1の`proresdec.c`は`chroma_sample_location`を設定せず、固定SDKのProton／DJIの`ffprobe`でも`chroma_location=unspecified`。左寄せは本実装の明示的な解釈であり、全収録素材の物理的位置が証明されたという意味ではない。過去の中央配置で得たRGB比較値は履歴として残し、新しい左寄せ判定と混同しない。

## IDCTジョブ表の例外復帰

CPUジョブ表の配置再構築・量子化値更新が始まった時点でdirtyを保持し、GPU `UpdateSubresource`呼出し後にだけ解除する。更新直後・upload前に例外を注入し、同一pipelineをPAUSEDへ戻してflushing seekした後、該当PTSの出力を正常復号の参照テクスチャと全画素比較する。これにより旧GPU表を「最新」と誤認する回帰を検出できる。

## 回帰判定

MSVC Releaseビルドを再実施。固定SDK CPU基準の全画素と、検査用staging上のDX11出力を比較した。通常のGStreamer出力にCPU読み戻しは加えていない。

| 回帰 | 対象 | 結果 |
|---|---:|---|
| DX11係数一致 | M1 11素材420枚、M2 18素材780枚、M3 24条件48枚、M4 9素材362枚、M5端数2素材4枚 | 全係数不一致0、画素最大差1 code |
| 全画素 | M1 420枚、M2 780枚、M3 alpha 48枚、M4 alpha 184枚・非alpha 180枚、M5動的24枚・端数RGB2枚 | YUV/RGB最大差1 code、alpha差0、動的変更全条件合格 |
| 左寄せRGB独立式 | M1/M2 29素材1,200枚＋公開Proton/DJI 3素材659枚 | 32素材1,859枚、RGB最大差1 code |
| 公開HQのD3D11Memory/EOS | Proton 50枚、DJI 4K24 129枚・4K60 480枚、SloMo 4K 120枚 | 4素材779枚、全件合格 |
| MSVC ASan parser変異 | 端数・M5を含む6 seed | 24,000件、ASan検出なし |
| スモーク | EOS×3、flushing seek×5、4種のインターレース | matrix=0/2のフォールバック、IDCT更新直後の例外→seek→全画素回復、8192²のDispatch算術を含め全件合格 |

4K 4:4:4＋alphaの通常device EOSと標準`d3d11convert`への1フレーム接続も通過。4K 4:2:2はM1/M2の実フレーム全画素検査を通過。HQ directの旧M5版との交互4組は、1080pが436.21→435.68 fps（−0.12%）、4Kが258.44→257.84 fps（−0.24%）で、両方とも−3%ゲートを通過した。長いログは`build/oss-prepublish/`、コミットする根拠は[最小集計JSON](../results/oss-prepublish-2026-09-25/summary.json)の1ファイルだけにした。この集計の`passed=true`を本修正の回帰判定とする。
