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
$rgbSmoke = Join-Path $build 'Release/d3d11_rgb_element_smoke.exe'
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$input1080 = Join-Path $root 'media/synthetic-1080p60-hq.mov'
$input2160 = Join-Path $root 'media/synthetic-2160p60-hq.mov'
$out = Join-Path $root $OutDir
foreach ($required in @($launcher, $smoke, $rgbSmoke, $ffmpeg, $input1080, $input2160,
    (Join-Path $pluginDirectory 'gstproresd3d11.dll'),
    (Join-Path $pluginDirectory 'prores_vld.cso'),
    (Join-Path $pluginDirectory 'prores_idct_unorm.cso'),
    (Join-Path $pluginDirectory 'prores_rgb.cso'))) {
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

    $smokeOutput = & $smoke $input1080 $input2160 2>&1
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

    foreach ($case in @(
        @{ Input = $input1080; Tag = '1080' },
        @{ Input = $input2160; Tag = '2160' }
    )) {
        # 元の合成ProResはtransfer/primariesが未指定。圧縮画素を変更せずMOVへBT.709を付与する。
        $tagged = Join-Path $build "synthetic-$($case.Tag)p60-bt709.mov"
        & $ffmpeg -hide_banner -loglevel error -i $case.Input -map 0:v:0 -c:v copy `
            -color_primaries bt709 -color_trc bt709 -colorspace bt709 -movflags +write_colr `
            -y $tagged
        if ($LASTEXITCODE) { throw "RGB用BT.709素材の作成に失敗: $($case.Input)" }
        $rgbOutput = & $rgbSmoke $tagged 180 2>&1
        $rgbCode = $LASTEXITCODE
        $rgbOutput | Set-Content -LiteralPath (Join-Path $out "proresd3d11-rgb-smoke-$($case.Tag).json") -Encoding utf8
        if ($rgbCode) { throw "RGB D3D11Memory・EOS・seek・寿命検査に失敗: $tagged" }
        $rgbResult = $rgbOutput | ConvertFrom-Json
        if (!$rgbResult.passed -or $rgbResult.frames -ne 180 -or !$rgbResult.seek -or
            !$rgbResult.retained_rgb_after_destroy) {
            throw "RGB検査結果が不完全: $tagged"
        }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        & $launcher -q -e filesrc "location=$($tagged -replace '\\', '/')" ! qtdemux ! `
            proresd3d11dec ! proresd3d11rgb ! fakesink sync=false
        $code = $LASTEXITCODE
        $timer.Stop()
        $rgbPerf = [ordered]@{
            passed = $code -eq 0
            exit_code = $code
            frames = 180
            sink = 'I422_10LE D3D11Memory -> RGB10A2_LE D3D11Memory direct'
            elapsed_seconds = $timer.Elapsed.TotalSeconds
            frames_per_second = 180 / $timer.Elapsed.TotalSeconds
        }
        $rgbPerf | ConvertTo-Json | Set-Content -LiteralPath `
            (Join-Path $out "proresd3d11-rgb-performance-$($case.Tag)-direct.json") -Encoding utf8
        if ($code) { throw "RGB D3D11Memory連続出力に失敗: $tagged" }
        Write-Host "RGB $($case.Tag)p60: $([math]::Round($rgbPerf.frames_per_second, 1)) fps、EOS・seek・寿命成功"
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
