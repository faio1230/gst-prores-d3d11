# ドットソースで実行する。変更は現在のPowerShellと子プロセスだけに限定する。
[CmdletBinding()]
param([string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64')
$pluginRoot = Split-Path $PSScriptRoot -Parent
$pluginDirectory = Join-Path $pluginRoot 'build/vs18/plugins/Release'
$pluginFFmpeg = Join-Path $pluginRoot 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
foreach ($required in @((Join-Path $pluginDirectory 'gstproresvk.dll'),
    (Join-Path $pluginDirectory 'gstproresd3d11.dll'),
    (Join-Path $pluginFFmpeg 'avcodec-62.dll'), (Join-Path $GStreamerRoot 'bin/gst-launch-1.0.exe'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
$env:PATH = "$pluginFFmpeg;$(Join-Path $GStreamerRoot 'bin');$env:PATH"
$env:GST_PLUGIN_PATH = if ($env:GST_PLUGIN_PATH) { "$pluginDirectory;$env:GST_PLUGIN_PATH" } else { $pluginDirectory }
$env:GST_REGISTRY = Join-Path $pluginRoot 'build/plugin-registry.bin'
Write-Host 'このPowerShellで proresvkdec と proresd3d11dec を利用できます。システム設定は変更していません。'
