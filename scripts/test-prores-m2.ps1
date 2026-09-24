# M2の全18素材をCPU全画素および固定SDK Vulkanと照合する。
[CmdletBinding()]
param(
    [string]$FixtureDir = 'results/prores-m2-2026-09-24',
    [string]$OutDir = 'results/prores-m2-2026-09-24/validation',
    [switch]$PixelOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$comparator = Join-Path $root 'scripts/compare-d3d11-real.py'
$fixtures = @(Get-Content -LiteralPath (Join-Path $root "$FixtureDir/fixtures.json") -Raw | ConvertFrom-Json) +
    @(Get-Content -LiteralPath (Join-Path $root "$FixtureDir/fixtures-4k.json") -Raw | ConvertFrom-Json)
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
if ($fixtures.Count -ne 18) { throw "M2素材数が不正: $($fixtures.Count)" }
$records = @()
foreach ($item in $fixtures) {
    $input = Join-Path $root $item.input
    if (!(Test-Path -LiteralPath $input) -or
        (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
        throw "素材SHA256が不一致: $($item.input)"
    }
    $name = [IO.Path]::GetFileNameWithoutExtension($input)
    $pixelPath = Join-Path $out "$name-pixels.json"
    & python $comparator $input --mode yuv --frames $item.frames --streaming --out $pixelPath `
        *> (Join-Path $out "$name-pixels-run.log")
    if ($LASTEXITCODE) { throw "全画素比較失敗: $name" }
    $pixel = Get-Content -LiteralPath $pixelPath -Raw | ConvertFrom-Json
    $maximum = ($pixel.channels | Measure-Object max_abs -Maximum).Maximum
    if (!$pixel.passed -or $pixel.frames -ne $item.frames -or $maximum -gt 1) {
        throw "画素差・枚数が不正: $name"
    }
    $vulkan = $null
    if (!$PixelOnly) {
        $format = "yuv$($item.chroma)p$($item.bit_depth)le"
        $vkLog = Join-Path $out "$name-vulkan.log"
        $md5 = Join-Path $out "$name-vulkan.framemd5"
        & $ffmpeg -hide_banner -y -loglevel verbose -init_hw_device vulkan=vk:0 `
            -filter_hw_device vk -hwaccel vulkan -hwaccel_output_format vulkan `
            -i $input -frames:v $item.frames -vf "hwdownload,format=$format" `
            -f framemd5 $md5 *> $vkLog
        $vkExit = $LASTEXITCODE
        $log = Get-Content -LiteralPath $vkLog -Raw
        $vkFrames = if (Test-Path -LiteralPath $md5) {
            @(Get-Content -LiteralPath $md5 | Where-Object { $_ -match '^\d+,' }).Count
        } else { 0 }
        if ($vkExit -ne 0 -or $log -notmatch 'Device 0 selected: NVIDIA GeForce RTX 3070' -or
            $log -notmatch 'pixfmt:vulkan' -or $vkFrames -ne $item.frames) {
            throw "固定SDK Vulkan全フレーム検査失敗: $name ($vkLog)"
        }
        $vulkan = [pscustomobject]@{ passed=$true; frames=$vkFrames; format=$format }
    }
    $records += [pscustomobject]@{
        input=$item.input; sha256=$item.sha256; origin=$item.origin
        fourcc=$item.fourcc; chroma=$item.chroma; bit_depth=$item.bit_depth
        width=$item.width; height=$item.height; frames=$item.frames
        cpu_dx11_pixel_max=[int]$maximum; cpu_dx11_all_planes_passed=$true
        vulkan=$vulkan
    }
    $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
    Write-Host "$name`: $($item.frames)枚、CPU-DX11最大差=$maximum、Vulkan=$($null -ne $vulkan)"
}
