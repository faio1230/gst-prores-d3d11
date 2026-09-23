# 公開ProRes素材の全フレームをD3D11Memoryで受け取り、EOSまで検査する。
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/vs18',
    [string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64',
    [string]$OutDir = 'results/verification-entropy-guard-2026-09-24/real'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root $BuildDir
$out = Join-Path $root $OutDir
$gstBin = Join-Path $GStreamerRoot 'bin'
$launcher = Join-Path $gstBin 'gst-launch-1.0.exe'
$bench = Join-Path $build 'Release/d3d11_plugin_bench.exe'
$ffprobe = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffprobe.exe'
$pluginDirectory = Join-Path $build 'plugins/Release'
$cases = @(
    @{ Name = 'proton-1080'; File = 'reference-proton-rec709-hq.mov' },
    @{ Name = 'dji-4k24'; File = 'reference-dji-nature-4k24-hq.mov' },
    @{ Name = 'dji-4k60'; File = 'reference-dji-nature-4k60-rec709-hq.mov' },
    # MOV時刻基準は2997/50fps、GStreamerのcapsは60000/1001fps。120枚で約2usずれる。
    @{ Name = 'slomo-4k60'; File = 'reference-slomo-4k-hq60-complete.mov'; PtsToleranceNs = 5000 }
)
foreach ($required in @($launcher, $bench, $ffprobe,
    (Join-Path $pluginDirectory 'gstproresd3d11.dll'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$savedPath = $env:PATH
$savedPluginPath = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$savedShaderDirectory = $env:PRORES_DX11_SHADER_DIR
try {
    $env:PATH = "$gstBin;$savedPath"
    $env:GST_PLUGIN_PATH = $pluginDirectory
    $env:GST_REGISTRY = Join-Path $out 'gst-registry.bin'
    $env:PRORES_DX11_SHADER_DIR = $null
    $records = @()
    foreach ($case in $cases) {
        $inputFile = Join-Path $root "media/$($case.File)"
        if (!(Test-Path -LiteralPath $inputFile)) { throw "入力素材がありません: $inputFile" }
        $countText = @(& $ffprobe -v error -select_streams v:0 `
            -show_entries stream=nb_frames -of default=noprint_wrappers=1:nokey=1 $inputFile)
        if ($LASTEXITCODE -ne 0 -or $countText.Count -ne 1) {
            throw "フレーム数を読めません: $inputFile"
        }
        $expected = 0
        if (![int]::TryParse($countText[0].Trim(), [ref]$expected) -or $expected -le 2) {
            throw "不正なフレーム数: $inputFile"
        }
        $csv = Join-Path $out "$($case.Name)-frames.csv"
        $benchLog = Join-Path $out "$($case.Name)-bench.log"
        $ptsToleranceNs = if ($case.ContainsKey('PtsToleranceNs')) { $case.PtsToleranceNs } else { 0 }
        $benchOutput = @(& $bench $inputFile dx11-direct $csv 1 0 $expected 0 0 0 $ptsToleranceNs 2>&1)
        $benchCode = $LASTEXITCODE
        $benchOutput | Set-Content -LiteralPath $benchLog -Encoding utf8
        if ($benchCode -ne 0) { throw "全フレームのD3D11Memory検査に失敗: $benchLog" }
        $benchJson = $benchOutput | Where-Object { $_ -match '^\{"passed":' } | Select-Object -Last 1
        if (!$benchJson) { throw "ベンチ結果JSONがありません: $benchLog" }
        $benchResult = $benchJson | ConvertFrom-Json
        if (!$benchResult.passed -or $benchResult.mode -ne 'dx11-direct' -or
            $benchResult.frames -ne $expected -or $benchResult.completed_loops -ne 1) {
            throw "全フレームの契約違反: $benchLog"
        }
        $eosLog = Join-Path $out "$($case.Name)-eos.log"
        $gstInput = $inputFile -replace '\\', '/'
        $eosOutput = @(& $launcher -q -e filesrc "location=$gstInput" ! qtdemux ! `
            proresd3d11dec ! 'video/x-raw(memory:D3D11Memory),format=I422_10LE' ! `
            fakesink sync=false 2>&1)
        $eosCode = $LASTEXITCODE
        $eosOutput | Set-Content -LiteralPath $eosLog -Encoding utf8
        if ($eosCode -ne 0) { throw "EOS検査に失敗: $eosLog" }
        $records += [ordered]@{
            input = $case.File
            expected_frames = $expected
            d3d11memory_frames = $benchResult.frames
            pts_tolerance_ns = $ptsToleranceNs
            eos = $true
            passed = $true
        }
        Write-Host "$($case.File): $expected 枚のD3D11Memory、EOS成功"
    }
    $summary = [ordered]@{
        passed = $true
        plugin = 'proresd3d11dec'
        output = 'I422_10LE D3D11Memory'
        ffprobe = 'repository-fixed FFmpeg 8.1 SDK'
        cases = $records
    }
    $summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
        (Join-Path $out 'summary.json') -Encoding utf8
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPluginPath
    $env:GST_REGISTRY = $savedRegistry
    $env:PRORES_DX11_SHADER_DIR = $savedShaderDirectory
}
