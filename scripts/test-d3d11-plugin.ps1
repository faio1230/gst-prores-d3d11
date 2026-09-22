# 純粋DX11プラグインを、FFmpeg/Vulkan DLLをPATHへ追加せず実パイプラインで検査する。
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/vs18',
    [string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64',
    [string]$OutDir = 'results'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root $BuildDir
$pluginDirectory = Join-Path $build 'plugins/Release'
$gstBin = Join-Path $GStreamerRoot 'bin'
$launcher = Join-Path $gstBin 'gst-launch-1.0.exe'
$smoke = Join-Path $build 'Release/d3d11_plugin_smoke.exe'
$input1080 = Join-Path $root 'media/synthetic-1080p60-hq.mov'
$input2160 = Join-Path $root 'media/synthetic-2160p60-hq.mov'
$out = Join-Path $root $OutDir
foreach ($required in @($launcher, $smoke, $input1080, $input2160,
    (Join-Path $pluginDirectory 'gstproresd3d11.dll'),
    (Join-Path $pluginDirectory 'prores_vld.cso'),
    (Join-Path $pluginDirectory 'prores_idct_unorm.cso'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
New-Item -ItemType Directory -Force $out | Out-Null
$savedPath = $env:PATH
$savedPluginPath = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$savedShaderDirectory = $env:PRORES_DX11_SHADER_DIR
try {
    $env:PATH = "$gstBin;$savedPath"
    $env:GST_PLUGIN_PATH = $pluginDirectory
    $env:GST_REGISTRY = Join-Path $build 'plugin-d3d11-test-registry.bin'
    Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue

    $smokeOutput = & $smoke $input1080 2>&1
    $smokeCode = $LASTEXITCODE
    $smokeOutput | Set-Content -LiteralPath (Join-Path $out 'proresd3d11-smoke.log') -Encoding utf8
    $smokeOutput | Write-Host
    if ($smokeCode) { throw 'D3D11プラグインのライフサイクル検査に失敗' }

    $cases = @(
        @{ Input = $input1080; Frames = 180; Output = 'proresd3d11-performance-1080-direct.json' },
        @{ Input = $input2160; Frames = 180; Output = 'proresd3d11-performance-2160-direct.json' }
    )
    foreach ($case in $cases) {
        $gstInput = $case.Input -replace '\\', '/'
        $timer = [Diagnostics.Stopwatch]::StartNew()
        & $launcher -q -e filesrc "location=$gstInput" ! qtdemux ! proresd3d11dec ! fakesink sync=false
        $code = $LASTEXITCODE
        $timer.Stop()
        $record = [ordered]@{
            passed = $code -eq 0
            exit_code = $code
            frames = $case.Frames
            sink = 'D3D11Memory direct'
            elapsed_seconds = $timer.Elapsed.TotalSeconds
            frames_per_second = $case.Frames / $timer.Elapsed.TotalSeconds
        }
        $record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out $case.Output) -Encoding utf8
        if ($code) { throw "D3D11Memory連続出力に失敗: $($case.Input)" }
        Write-Host "$(Split-Path $case.Input -Leaf): $([math]::Round($record.frames_per_second, 1)) fps、EOS成功"
    }
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPluginPath
    $env:GST_REGISTRY = $savedRegistry
    if ($null -eq $savedShaderDirectory) {
        Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    } else {
        $env:PRORES_DX11_SHADER_DIR = $savedShaderDirectory
    }
}
