# M2全18素材のD3D11Memory EOS/seekとRGBのEOS/seek/保持buffer寿命を検査する。
[CmdletBinding()]
param([string]$OutDir = 'results/prores-m2-2026-09-24/lifecycle')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fixtureDir = Join-Path $root 'results/prores-m2-2026-09-24'
$fixtures = @(Get-Content -LiteralPath (Join-Path $fixtureDir 'fixtures.json') -Raw | ConvertFrom-Json) +
    @(Get-Content -LiteralPath (Join-Path $fixtureDir 'fixtures-4k.json') -Raw | ConvertFrom-Json)
$rgbFixtures = @(Get-Content -LiteralPath (Join-Path $fixtureDir 'fixtures-rgb.json') -Raw | ConvertFrom-Json)
$bench = Join-Path $root 'build/vs18/Release/d3d11_plugin_bench.exe'
$rgb = Join-Path $root 'build/vs18/Release/d3d11_rgb_element_smoke.exe'
$gstBin = 'C:/Program Files/gstreamer/1.0/msvc_x86_64/bin'
$out = Join-Path $root $OutDir
if ($fixtures.Count -ne 18 -or $rgbFixtures.Count -ne 18) { throw 'M2の18素材が不足' }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$savedPath = $env:PATH
$savedPlugin = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$records = @()
try {
    $env:PATH = "$gstBin;$savedPath"
    $env:GST_PLUGIN_PATH = Join-Path $root 'build/vs18/plugins/Release'
    $env:GST_REGISTRY = Join-Path $root 'build/vs18/prores-m2-lifecycle-registry.bin'
    for ($index = 0; $index -lt $fixtures.Count; ++$index) {
        $item = $fixtures[$index]
        $rgbItem = $rgbFixtures[$index]
        $input = Join-Path $root $item.input
        $rgbInput = Join-Path $root $rgbItem.input
        if ((Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256 -or
            (Get-FileHash -LiteralPath $rgbInput -Algorithm SHA256).Hash.ToLowerInvariant() -ne $rgbItem.sha256 -or
            $rgbItem.source_sha256 -ne $item.sha256) { throw "素材対応が不正: $($item.input)" }
        $name = [IO.Path]::GetFileNameWithoutExtension($input)
        $csv = Join-Path $out "$name-direct.csv"
        $directLines = @(& $bench $input dx11-direct $csv 2 0 $item.frames 2 0 0 2>&1 | ForEach-Object { "$_" })
        $directLines | Set-Content -LiteralPath (Join-Path $out "$name-direct.log") -Encoding utf8
        if ($LASTEXITCODE) { throw "D3D11Memory EOS/seek失敗: $name" }
        $direct = $directLines | Select-Object -Last 1 | ConvertFrom-Json
        if (!$direct.passed -or $direct.completed_loops -ne 2 -or
            $direct.frames -ne 2 * $item.frames -or $direct.seek_count -lt 2 -or
            $direct.gpu_completion_wait) { throw "D3D11Memory寿命JSON不合格: $name" }
        $rgbLines = @(& $rgb $rgbInput $item.frames 2>&1 | ForEach-Object { "$_" })
        $rgbLines | Set-Content -LiteralPath (Join-Path $out "$name-rgb.log") -Encoding utf8
        if ($LASTEXITCODE) { throw "RGB EOS/seek失敗: $name" }
        $rgbResult = $rgbLines | Select-Object -Last 1 | ConvertFrom-Json
        if (!$rgbResult.passed -or $rgbResult.frames -ne $item.frames -or
            !$rgbResult.seek -or !$rgbResult.retained_rgb_after_destroy) {
            throw "RGB EOS/seek/保持buffer寿命JSON不合格: $name"
        }
        $records += [pscustomobject]@{
            input=$item.input; sha256=$item.sha256; frames=$item.frames
            d3d11memory_frames=$direct.frames; d3d11memory_eos_cycles=$direct.completed_loops
            d3d11memory_seeks=$direct.seek_count; rgb_frames=$rgbResult.frames
            rgb_seek=$rgbResult.seek; retained_rgb_after_destroy=$rgbResult.retained_rgb_after_destroy
            passed=$true
        }
        $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
        Write-Host "$name`: D3D11Memory EOS×2/seek、RGB EOS/seek/寿命成功"
    }
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPlugin
    $env:GST_REGISTRY = $savedRegistry
}
