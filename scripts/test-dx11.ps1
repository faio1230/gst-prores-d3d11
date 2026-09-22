[CmdletBinding()]
param(
    [string]$BuildDir = 'build/vs18',
    [string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64',
    [string]$OutDir = 'results'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root $BuildDir
$out = Join-Path $root $OutDir
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$validator = Join-Path $build 'Release/prores_dx11_coeff.exe'
$probe = Join-Path $build 'Release/d3d11_memory_probe.exe'
foreach ($path in @($ffmpeg, $validator, $probe)) {
    if (!(Test-Path -LiteralPath $path)) { throw "必要な固定依存物または実行ファイルがありません: $path" }
}
New-Item -ItemType Directory -Force $out | Out-Null
$savedPath = $env:PATH
try {
    $env:PATH = "$ffmpeg;$(Join-Path $GStreamerRoot 'bin');$savedPath"
    $cases = @(
        @{ Input = 'media/synthetic-1080p60-hq.mov'; Output = 'dx11-decode-1080.json' },
        @{ Input = 'media/synthetic-2160p60-hq.mov'; Output = 'dx11-decode-2160.json' }
    )
    foreach ($case in $cases) {
        $inputPath = Join-Path $root $case.Input
        if (!(Test-Path -LiteralPath $inputPath)) { throw "合成素材がありません: $inputPath" }
        $json = & $validator $inputPath (Join-Path $root 'src/prores_vld.hlsl')
        if ($LASTEXITCODE) { throw "DX11復号検査に失敗: $($case.Input)" }
        $json | Set-Content -LiteralPath (Join-Path $out $case.Output) -Encoding utf8
        $parsed = $json | ConvertFrom-Json
        if (!$parsed.passed -or $parsed.mismatches -ne 0 -or $parsed.shader_errors -ne 0) {
            throw "DX11復号検査JSONが不合格: $($case.Input)"
        }
        Write-Host "$($case.Input): 係数完全一致、画素最大差=$((($parsed.pixel_differences | Measure-Object max_abs -Maximum).Maximum))"
    }
    $probeJson = & $probe
    if ($LASTEXITCODE) { throw 'GStreamer D3D11Memory形式検査に失敗' }
    $probeJson | Set-Content -LiteralPath (Join-Path $out 'd3d11-memory-probe.json') -Encoding utf8
    $probeResult = $probeJson | ConvertFrom-Json
    if (!$probeResult.passed -or $probeResult.memories -ne 3 -or !$probeResult.all_planes_uav) {
        throw 'GStreamer D3D11Memory形式検査JSONが不合格'
    }
    Write-Host 'I422_10LE D3D11Memory: 3面R16_UNORM、全UAV作成成功'
} finally {
    $env:PATH = $savedPath
}
