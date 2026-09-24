# 422系4プロファイルの係数・全画素・D3D11Memory EOS/seek/RGBを素材別に検査する。
[CmdletBinding()]
param(
    [string]$OutDir = 'results/profile-422-expansion-2026-09-24',
    [string]$FixturesFile = 'fixtures.json',
    [string]$SummaryFile = 'verification-summary.json'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$fixtures = Join-Path $out $FixturesFile
$ffmpegBin = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffmpeg = Join-Path $ffmpegBin 'ffmpeg.exe'
$gstBin = 'C:/Program Files/gstreamer/1.0/msvc_x86_64/bin'
$plugin = Join-Path $root 'build/vs18/plugins/Release'
$coeff = Join-Path $root 'build/vs18/Release/prores_dx11_coeff.exe'
$rgbSmoke = Join-Path $root 'build/vs18/Release/d3d11_rgb_element_smoke.exe'
$launch = Join-Path $gstBin 'gst-launch-1.0.exe'
$python = (Get-Command python -ErrorAction Stop).Source
foreach ($required in @($fixtures, $ffmpeg, $plugin, $coeff, $rgbSmoke, $launch)) {
    if (!(Test-Path -LiteralPath $required)) { throw "検証の依存物がありません: $required" }
}
$savedPath = $env:PATH
$savedPluginPath = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$savedShaderDirectory = $env:PRORES_DX11_SHADER_DIR
$records = @()
try {
    $env:PATH = "$ffmpegBin;$gstBin;$savedPath"
    $env:GST_PLUGIN_PATH = $plugin
    $env:GST_REGISTRY = Join-Path $root 'build/vs18/profile-422-registry.bin'
    Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    foreach ($item in (Get-Content -LiteralPath $fixtures -Raw | ConvertFrom-Json)) {
        $inputFile = Join-Path $root $item.input
        $tag = "$($item.origin)-$($item.profile)"
        if ((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne
            $item.sha256) { throw "素材SHA256が変わりました: $tag" }
        $vkMd5 = Join-Path $out "vulkan-$tag.framemd5"
        $vkLog = Join-Path $out "vulkan-$tag.log"
        & $ffmpeg -hide_banner -y -loglevel verbose -init_hw_device vulkan=vk:0 `
            -filter_hw_device vk -hwaccel vulkan -hwaccel_output_format vulkan `
            -i $inputFile -frames:v $item.frames -vf 'hwdownload,format=yuv422p10le' `
            -f framemd5 $vkMd5 *> $vkLog
        if ($LASTEXITCODE) { throw "Vulkan GPU復号に失敗: $tag ($vkLog)" }
        $vkText = Get-Content -LiteralPath $vkLog -Raw
        $vkFrames = @(Get-Content -LiteralPath $vkMd5 | Where-Object { $_ -match '^\d+,' }).Count
        if ($vkText -notmatch 'Device 0 selected: NVIDIA GeForce RTX 3070' -or
            $vkText -notmatch 'pixfmt:vulkan' -or $vkFrames -ne $item.frames) {
            throw "Vulkan GPUフレームまたは枚数が不一致: $tag ($vkLog)"
        }
        $coeffLog = Join-Path $out "coeff-$tag.log"
        & $coeff $inputFile (Join-Path $root 'src/prores_vld.hlsl') *> $coeffLog
        if ($LASTEXITCODE) { throw "係数または先頭画素の検査に失敗: $tag ($coeffLog)" }
        $coeffResult = Get-Content -LiteralPath $coeffLog -Tail 1 | ConvertFrom-Json
        $coeffMax = ($coeffResult.pixel_differences | Measure-Object max_abs -Maximum).Maximum
        if (!$coeffResult.passed -or $coeffResult.mismatches -ne 0 -or
            $coeffResult.shader_errors -ne 0 -or $coeffMax -gt 1) {
            throw "係数・先頭画素のJSONが不合格: $tag"
        }
        $pixelJson = Join-Path $out "pixel-$tag.json"
        $pixelLog = Join-Path $out "pixel-$tag.log"
        & $python (Join-Path $root 'scripts/compare-d3d11-real.py') $inputFile `
            --mode yuv --out $pixelJson *> $pixelLog
        if ($LASTEXITCODE) { throw "全30フレームの画素比較に失敗: $tag ($pixelLog)" }
        $pixels = Get-Content -LiteralPath $pixelJson -Raw | ConvertFrom-Json
        $pixelMax = ($pixels.channels | Measure-Object max_abs -Maximum).Maximum
        if (!$pixels.passed -or $pixels.frames -ne $item.frames -or $pixelMax -gt 1) {
            throw "全画素比較のJSONが不合格: $tag"
        }
        $rgbLog = Join-Path $out "rgb-$tag.log"
        & $rgbSmoke $inputFile $item.frames *> $rgbLog
        if ($LASTEXITCODE) { throw "RGB/EOS/seek検査に失敗: $tag ($rgbLog)" }
        $rgb = Get-Content -LiteralPath $rgbLog -Tail 1 | ConvertFrom-Json
        if (!$rgb.passed -or $rgb.frames -ne $item.frames -or !$rgb.seek -or
            !$rgb.retained_rgb_after_destroy) { throw "RGB/EOS/seek JSONが不合格: $tag" }
        $eosLog = Join-Path $out "direct-eos-$tag.log"
        & $launch -q -e filesrc "location=$($inputFile -replace '\\','/')" ! qtdemux ! `
            proresd3d11dec ! 'video/x-raw(memory:D3D11Memory),format=I422_10LE' ! `
            fakesink sync=false *> $eosLog
        if ($LASTEXITCODE) { throw "D3D11Memory直接EOS検査に失敗: $tag ($eosLog)" }
        $records += [pscustomobject]@{
            profile = $item.profile
            fourcc = $item.fourcc
            origin = $item.origin
            source = $item.input
            coefficient_frames = 1
            coefficient_mismatches = $coeffResult.mismatches
            shader_errors = $coeffResult.shader_errors
            pixel_frames = $pixels.frames
            pixel_max_abs = $pixelMax
            vulkan_gpu = 'NVIDIA GeForce RTX 3070'
            vulkan_frames = $vkFrames
            direct_d3d11memory_eos = $true
            rgb_d3d11memory_eos = $true
            flushing_seek = $rgb.seek
            retained_rgb_after_destroy = $rgb.retained_rgb_after_destroy
            passed = $true
        }
        $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath `
            (Join-Path $out $SummaryFile) -Encoding utf8
        Write-Host "$tag`: 係数完全一致、全$($pixels.frames)枚の画素差最大$pixelMax、EOS/seek/RGB成功"
    }
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPluginPath
    $env:GST_REGISTRY = $savedRegistry
    if ($null -eq $savedShaderDirectory) {
        Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    } else { $env:PRORES_DX11_SHADER_DIR = $savedShaderDirectory }
}
