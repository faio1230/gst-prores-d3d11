# 純粋DX11版だけを現在のPowerShellへ設定する。FFmpeg/VulkanのPATHは追加しない。
[CmdletBinding()]
param([string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64')
$pluginRoot = Split-Path $PSScriptRoot -Parent
$pluginDirectory = Join-Path $pluginRoot 'build/vs18/plugins/Release'
foreach ($required in @((Join-Path $pluginDirectory 'gstproresd3d11.dll'),
    (Join-Path $pluginDirectory 'prores_vld.cso'),
    (Join-Path $pluginDirectory 'prores_idct_unorm.cso'),
    (Join-Path $GStreamerRoot 'bin/gst-launch-1.0.exe'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
$env:PATH = "$(Join-Path $GStreamerRoot 'bin');$env:PATH"
$env:GST_PLUGIN_PATH = if ($env:GST_PLUGIN_PATH) {
    "$pluginDirectory;$env:GST_PLUGIN_PATH"
} else {
    $pluginDirectory
}
$env:GST_REGISTRY = Join-Path $pluginRoot 'build/plugin-d3d11-registry.bin'
Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
Write-Host 'このPowerShellで純粋DX11版proresd3d11decを利用できます。FFmpeg/VulkanのPATHは追加していません。'
