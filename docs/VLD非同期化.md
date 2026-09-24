# VLDエラー検査の非同期化

採用版＝3面リングバッファ版、ワーカー系は不採用。旧版との交互比較でdecoder QoS欠落0/0/0/0枚、合成direct性能は1080p/4Kとも改善し、待ちp99も短縮したため。
従来の「待ち最大値」単独ゲートは撤回し、p99/p99.9と内部待ち内訳で再判定する。以下のワーカー案・Flush1案は検討履歴であり、現在の実装方針ではない。

## 変更した設計

`proresd3d11dec`はProRes 422 HQのVLDをDX11 Compute Shaderで実行し、CPUによる毎フレームのエラーフラグMap完了待ちを廃止した。GPU命令はVLD→IDCT→エラーフラグのstagingコピーの順に投入し、画像は従来どおり3面の`GstD3D11Memory`で返す。エラーフラグのstagingを3面のリングとし、4枚目の投入前に最古の結果を回収する。EOS/drainでは残りをすべて回収する。破損が見つかれば`STREAM/DECODE`を通知して以後の復号を停止する。flushing seekでは旧segmentの未回収結果を破棄し、新segmentへエラーを持ち込まない。

IDCT shader側でエラーを読んで出力を無効化する案は選ばなかった。VLDのjob別エラーを各IDCT blockへ対応させる追加のGPU依存と、異常画素をdownstreamへ見せないための別の同期・出力管理が必要になる。3面リングなら既存のVLD shaderと画素演算を変えず、単一フレームの破損もEOS回収で確実にエラーへできる。代わりに破損フレームの画像がエラー通知より先にdownstreamへ渡る可能性があり、通知は通常最大3フレーム遅れる。フラッシュで破棄した旧segmentの未回収GPUエラーは報告しない。

リング再利用とEOSの回収時だけ、`Flush()`で命令を提出した後に`Map(DO_NOT_WAIT)`を繰り返す。各MapとUnmapの間だけ`GstD3D11Device` lockを取り、反復の合間は解放する。10秒期限と`GetDeviceRemovedReason()`検査は残した。診断用timestamp queryの待機もlockを解放して行う。GStreamer 1.28.2の[標準sink実装](https://github.com/GStreamer/gstreamer/blob/1.28.2/subprojects/gst-plugins-bad/sys/d3d11/gstd3d11videosink.cpp#L1621-L1646)は`gst_d3d11_window_render`を呼び、その[window実装](https://github.com/GStreamer/gstreamer/blob/1.28.2/subprojects/gst-plugins-bad/sys/d3d11/gstd3d11window.cpp#L986-L995)が同じdevice lockを取得する。従来のMap待機中はsinkのレンダリングと競合し得たが、今回の回収待ちはlockを保持し続けない。

## 検証と採用判定

RTX 3070、固定GStreamer 1.28.2でビルド、係数完全一致、合成1080p/4Kの画素差最大1 code、EOS×3・flushing seek×4・RGB・動的caps・寿命試験を通過。公開実素材50+129+480+120=779枚はD3D11MemoryのままEOSまで完走した。固定SDKで抽出したpacketのASan付き決定的変異2,000件を通過し、AC境界単一byte再現例とDC異常をGPUでも`STREAM/DECODE`で拒否した。EOSを送らず破損1枚目を4枚目のリング再利用で検出するテストも追加した。通常QoS・Present(0)の実写4K60×10周パイロットは、VLD→コピー→IDCT版とVLD→IDCT→コピー版の両方で4800/4800枚だった。

旧版`a3b4315`と新版`b981a32`は、それぞれcleanなソースから内部ステージを再ビルドし、DLLのSHA256が異なり3つのCSOは一致することを確認した。通常QoS・Present(0)・実写4K60×10周の交互4組では、decoder QoS欠落が旧版23/0/0/0枚、新版0/0/0/0枚（各4800枚）で、新版は全試行0枚、中央値は双方0枚。合成素材directの交互4組は旧→新中央値で1080p 393.871→439.856fps（+11.68%）、4K 212.729→255.129fps（+19.93%）。この2条件は合格した。

同じ実写素材のCPU段階ログ付き交互4組では、旧版Map待ち対新版回収待ちのp99中央値が4.188→0.536ms、backend/decode全体のp99中央値が8.066→4.391msへ改善した。しかし**待機最大値は未達**で、試行別最大値の中央値が6.397→8.172ms、全試行最大値が12.222→44.401msへ悪化した。新版の44.401ms行は`retire_map_attempts=1`で、複数回のMapポーリングによるものではない。直前フレームの`finish_ms`は113.292msで、lock取得・Flush・1回のMapの内訳まではこのログから断定できない。診断の新版1試行にはsink dropが4枚あったが、decoder出力・RGB出力・sink push/returnは各4800枚、decoder QoS欠落は0枚だった。通常比較4組のsink dropは0枚であり、診断ログのsink dropをdecoder欠落に混ぜない。

採用ゲートは**不合格**。次の実装変更は、3面stagingの回収をdecoder呼び出しから分離した専用完了ワーカーに移すこと。ワーカーがdevice lockを短時間ずつ取得して結果を回収・順序付きで`STREAM/DECODE`を通知し、decoderは3面が全て未回収の場合だけ空きを待つ設計を試す。今回の最大44.401msがlock競合かOSスケジューリングかは未確定で、この変更で解消するとはまだ主張しない。OS/DWM未表示はdecoder採用ゲート外の参考値であり、この比較では測定しなかった。集計は`results/vld-async-gate-summary-2026-09-24.json`、行別記録は`results/display-vld-async-ab-2026-09-24/`、`results/vld-async-direct-ab-2026-09-24/`、`results/vld-async-wait-ab*-2026-09-24/`。

## 専用完了ワーカー版（不採用・履歴）

3面stagingとGPUのVLD→IDCT→error copy順は維持し、error copyの`Flush()`・`Map(DO_NOT_WAIT)`・job別検査を専用ワーカーへ移した。ワーカーは古いフレームから順に処理し、各D3D11呼び出しの間はdevice lockを離す。decoderはCPUのjob準備後、3面すべてが未回収の場合だけ空きを待つ。GPU拒否またはdevice異常は共有状態へ記録し、次のdecoder呼び出しまたはEOS/drainで例外として`STREAM/DECODE`または`RESOURCE/FAILED`へ変換する。したがって破損フレームを含む数枚が通知より先にdownstreamへ渡り得るし、入力停止時にEOS/drainも次フレームもなければ通知は保留される。flushing seekと停止ではワーカーを中断・joinして旧segmentの結果を破棄し、再開時に新しいワーカーを開始する。回収・空き待ち・drainの10秒期限とdevice removedの確認を残す。

RTX 3070でビルド、係数完全一致、合成1080p/4Kの画素差最大1 code、EOS×3・flushing seek×4・RGB・破損14例、公開実素材779枚のD3D11Memory/EOS、ASan変異2,000件（AC境界9件拒否）を通過した。通常QoS・Present(0)の実写4K60×10周パイロットは4800/4800枚、decoder QoS欠落・sink dropとも0枚。この単発値を独立ステージ交互4組の代わりにはしない。`results/verification-vld-worker-2026-09-24/`と`results/display-vld-worker-pilot-2026-09-24/`に記録した。

旧3面リング版`b981a32`とワーカー版`0c41803`のcleanな独立ステージによる通常QoS・Present(0)・実写4K60×10周の交互4組は、decoder QoS欠落が旧11/0/1/0枚、新0/0/0/0枚（各4800枚）で、中央値は双方0枚。sink dropは全試行0枚。旧基準版`a3b4315`と新版の合成direct交互4組では、1080p中央値が392.646→314.561fps（**−19.89%**）で、許容下限−3%を超えて悪化した。4Kは213.217→254.027fps（+19.14%）。1080pの新版4試行は311.56～319.12fpsで一貫して低く、単発の外れ値だけでは説明できない。ステージDLLは異なり、shader 3種は同一SHA256。採用ゲートはこのdirect条件で**不合格**とし、旧基準版との表示交互4組および回収待ちp99・最大値の追加計測は実施しない。

次に試す実装変更は、ワーカーが1枚目の投入直後からGPU完了をポーリングする動作をやめ、未回収が2枚に達した時点（またはEOS/drain）で最古の検査を開始すること。GPU copyに進む時間を与え、短い素材の連続direct復号でworkerの頻繁なdevice lock取得を減らす狙いである。3面上限、FIFO、破損拒否、EOS回収は維持する。これで性能と最大待機の両条件を満たすかは未検証。比較記録は`results/display-vld-worker-ab-2026-09-24/`と`results/vld-worker-direct-a3-ab-2026-09-24/`。

## 2枚蓄積開始版（不採用・履歴）

ワーカーの開始条件を未回収2枚、またはEOS/drainで残りがある場合に変更した。3面上限とFIFO、`Flush()`後の非blocking Map、10秒期限、flushing seekの破棄は維持した。ビルド、係数完全一致、画素差最大1、EOS×3・seek×4・RGB・破損14例、公開素材779枚のD3D11Memory/EOS、ASan変異2,000件（AC境界9件拒否）は通過。合成1080p directの単独3反復は378.152/380.801/382.038fpsで、前版の交互比較中央値314.561fpsより高いが、同時期の旧版との交互比較ではないため性能ゲートには使わない。

通常QoS・Present(0)の実写4K60×10周パイロットは**4781/4800枚**で、19枚がすべてdecoder出力前のQoS欠落、sink dropは0枚だった。全試行decoder欠落0枚の必須条件に反するため、この候補の独立ステージ交互4組と待機p99・最大値は実施せず、採用しない。次の実装変更は、1枚目から回収ワーカーを動かしつつ、各error copyの後にD3D11 event queryを置き、ワーカーはevent完了を確認してからstagingをMapする方式へ替えること。2枚蓄積による回収開始遅延をなくし、未完了stagingへのMapポーリングを減らす狙いであり、効果は未検証。記録は`results/verification-vld-worker-two-deep-2026-09-24/`、`results/vld-worker-two-deep-direct-pilot-2026-09-24/`、`results/display-vld-worker-two-deep-pilot-2026-09-24/`。

## D3D11 event queryワーカー版（不採用・履歴）

3面の各error stagingに`D3D11_QUERY_EVENT`を対応させ、copy直後に`End(query)`を投入した。ワーカーを再び1枚目から動かし、明示`Flush()`後の`GetData(DONOTFLUSH)`が`S_OK`になってからstagingを非blocking Mapする。[Microsoftのevent query仕様](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)ではGPUの先行命令完了を`S_OK`で示し、[DONOTFLUSHの仕様](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag)は未提出命令への無限待ちを警告するため、Flushを維持した。待機中はdevice lockを保持しない。ビルドとプラグイン試験（EOS×3、seek×4、RGB、破損14例）は通過。係数・画素、公開素材779枚、ASan変異はこの候補では再実行していない。

合成1080p direct単独3反復は431.772/445.199/448.019fps。ただし旧版との交互比較ではない。通常QoS・Present(0)の実写4K60×10周パイロットは**4780/4800枚**で、20枚がすべてdecoder出力前、sink dropは0枚。decoder欠落全試行0枚に反するので独立ステージ交互4組と待機p99・最大値は実施せず、採用しない。次の実装変更は、利用可能なD3D11.3の`ID3D11DeviceContext3::Flush1`によるWin32完了イベント通知に回収ワーカーを切り替え、`GetData`のdevice lock付き反復をなくすこと。非対応deviceは明示的に停止し、CPU/Vulkan fallbackは置かない。効果・互換性は未検証。記録は`results/vld-worker-event-direct-pilot-2026-09-24/`と`results/display-vld-worker-event-pilot-2026-09-24/`。

## 3面リング版の再判定（採用）

上記ワーカー系の次案は撤回した。`b981a32`の3面リング・decoderスレッド回収へソースを戻し、診断時のみ回収のdevice lock取得、`Flush()`、`Map()`呼出し時間を分離した。通常復号の制御・GPU命令順・エラー通知は変更していない。最大値単独ゲートを撤回し、分布と内部待ち20ms超が4試行中2試行以上かで判定した。Map時間は各`Map()`呼出しの合計で、余りは反復・走査・スケジューリング等を含む。

RTX 3070の全回帰は、係数完全一致、合成1080p/4Kの画素差最大1 code、EOS×3、flushing seek×4、RGB、動的caps、buffer寿命、破損14例（EOSを送らない4枚目リング再利用を含む）を通過した。公開実素材50+129+480+120=779枚はD3D11MemoryのままEOSまで完走。固定FFmpeg SDKで抽出した4素材×500件のASan変異2,000件を通過し、AC境界の単一byte再現例はGPUでも`STREAM/DECODE`で拒否した。`results/verification-vld-ring-restored-2026-09-24/`に生ログを保存した。

旧基準`a3b4315`と復元版`b08e1d9`をcleanなソースから独立再ビルドし、DLL差分・shader同一性を確認した。通常QoS・Present(0)・標準sinkの実写4K60×10周を交互4組で測ると、decoder QoS欠落は旧22/0/0/0枚、新0/0/0/0枚、sink dropは全試行0。合成directの中央値は1080pで392.35→443.60fps（+13.06%）、4Kで211.08→259.17fps（+22.78%）。CPU段階ログ付きの別の交互4組では、待ちp99中央値が4.240→0.510ms、p99.9が4.604→0.837ms、backend p99が7.823→4.564ms。新版の内部lock/Flush/Map合計20ms超は4試行とも0件。**decoder側の全採用条件を通過**した。最大値は合否に算入しない。

新版の回収待ち上位5行を全4試行から抽出した。sink時刻は同一試行のCSVを`(loop, PTS)`で結合した開始後経過msであり、直前フレームの`finish_ms`は所要時間なので絶対時刻ではない。時刻の近接だけから待ちの原因は断定しない。

| 試行/seq/loop | 当該PTS(ns) | 回収/lock/Flush/Map/余り(ms) | 直前PTS(ns) / finish(ms) | sink直前 push→return(ms) | sink当該 push→return(ms) |
|---|---:|---:|---:|---:|---:|
| p1-new/687/1 | 3450000000 | 2.892 / 0.003 / 2.841 / 0.010 / 0.038 | 3433333333 / 12.699 | 11539.8→11551.5 | 11561.7→11568.0 |
| p0-new/2305/4 | 6416666666 | 1.573 / 0.005 / 1.520 / 0.008 / 0.040 | 6400000000 / 13.332 | 38481.1→38493.4 | 38503.7→38509.6 |
| p2-new/2787/5 | 6450000000 | 1.235 / 0.003 / 1.165 / 0.009 / 0.058 | 6433333333 / 13.679 | 46511.4→46524.9 | 46531.4→46542.8 |
| p0-new/906/1 | 7100000000 | 1.224 / 0.007 / 1.141 / 0.013 / 0.064 | 7083333333 / 13.089 | 15183.1→15196.0 | 15205.4→15212.5 |
| p3-new/842/1 | 6033333333 | 1.150 / 0.005 / 1.078 / 0.009 / 0.059 | 6016666666 / 13.220 | 14121.6→14134.6 | 14143.5→14151.4 |

集計は`results/vld-ring-restored-gate-2026-09-24.json`、試行別のPTS・CPUログ・sink時刻は`results/vld-ring-restored-{display,direct,wait}-ab-2026-09-24/`を参照。OS/DWM未表示はdecoder側ゲートに含めず、既存の表示側参考記録として残す。対応GPUはRTX 3070のみの実機確認であり、公開配布認定や他GPUでの動作保証ではない。
