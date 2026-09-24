# M2の8交差条件を1080p/4K directで測り、同素材の固定SDK Vulkanも別境界で記録する。
[CmdletBinding()]
param([string]$OutDir = 'results/prores-m2-2026-09-24/performance')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$fixtureDir = Join-Path $root 'results/prores-m2-2026-09-24'
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$bench = Join-Path $root 'scripts/benchmark-d3d11-plugin.py'
$fixtures1080 = @(Get-Content -LiteralPath (Join-Path $fixtureDir 'fixtures.json') -Raw | ConvertFrom-Json |
    Where-Object origin -EQ 'synthetic')
$fixtures4k = @(Get-Content -LiteralPath (Join-Path $fixtureDir 'fixtures-4k.json') -Raw | ConvertFrom-Json)
$fixtures = $fixtures1080 + $fixtures4k
if ($fixtures.Count -ne 16 -or !(Test-Path -LiteralPath $ffmpeg)) {
    throw 'M2の16合成素材または固定SDKが不足'
}
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
foreach ($item in $fixtures) {
    $input = Join-Path $root $item.input
    if ((Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
        throw "素材SHA256が不一致: $($item.input)"
    }
    $tag = [IO.Path]::GetFileNameWithoutExtension($input)
    $trial = Join-Path $out "dx11-$tag"
    & python $bench $input --modes dx11-direct --repeats 3 --loops 5 --warmup 0 `
        --frames-per-loop $item.frames --seeks 0 --startups 0 `
        --build-dir (Join-Path $root 'build/vs18') --out $trial `
        *> (Join-Path $out "dx11-$tag.log")
    if ($LASTEXITCODE) { throw "DX11 direct測定失敗: $tag" }
    $dx = @(Get-Content -LiteralPath (Join-Path $trial 'medians.json') -Raw | ConvertFrom-Json)[0]
    if ($dx.mode -ne 'dx11-direct' -or $dx.repeats -ne 3 -or
        $dx.steady_fps -le 0 -or $dx.gpu_completion_wait) {
        throw "DX11 direct測定条件が不正: $tag"
    }
    $format = "yuv$($item.chroma)p$($item.bit_depth)le"
    $vkRecords = @()
    foreach ($method in @('wrapped', 'download')) {
        for ($repeat = 0; $repeat -lt 3; ++$repeat) {
            $logPath = Join-Path $out "vulkan-$tag-$method-r$repeat.log"
            $expected = $item.frames * 5
            $arguments = @('-hide_banner','-nostats','-loglevel','verbose','-benchmark',
                '-init_hw_device','vulkan=vk:0','-filter_hw_device','vk',
                '-hwaccel','vulkan','-hwaccel_output_format','vulkan',
                '-stream_loop','4','-i',$input,'-frames:v',[string]$expected)
            if ($method -eq 'wrapped') {
                $arguments += @('-noautoscale','-c:v','wrapped_avframe')
            } else {
                $arguments += @('-vf',"hwdownload,format=$format")
            }
            $arguments += @('-f','null','NUL')
            & $ffmpeg @arguments *> $logPath
            $code = $LASTEXITCODE
            $log = Get-Content -LiteralPath $logPath -Raw
            $encoded = [regex]::Matches($log, '(\d+) frames encoded')
            $count = if ($encoded.Count) { [int]$encoded[$encoded.Count-1].Groups[1].Value } else { 0 }
            $timing = [regex]::Matches($log, 'bench: utime=[0-9.]+s stime=[0-9.]+s rtime=([0-9.]+)s')
            $seconds = if ($timing.Count) {
                [double]::Parse($timing[$timing.Count-1].Groups[1].Value,
                    [Globalization.CultureInfo]::InvariantCulture)
            } else { 0 }
            if ($code -ne 0 -or $log -notmatch 'Device 0 selected: NVIDIA GeForce RTX 3070' -or
                $log -notmatch 'pixfmt:vulkan' -or $count -ne $expected -or $seconds -le 0) {
                throw "Vulkan測定条件が不正: $tag/$method/$repeat ($logPath)"
            }
            $vkRecords += [pscustomobject]@{
                method=$method; repeat=$repeat; frames=$count; seconds=$seconds; fps=$count/$seconds
            }
        }
    }
    $wrapped = @($vkRecords | Where-Object method -EQ 'wrapped' | ForEach-Object fps | Sort-Object)[1]
    $download = @($vkRecords | Where-Object method -EQ 'download' | ForEach-Object fps | Sort-Object)[1]
    $records += [pscustomobject]@{
        input=$item.input; sha256=$item.sha256; fourcc=$item.fourcc
        chroma=$item.chroma; bit_depth=$item.bit_depth
        width=$item.width; height=$item.height; frames_per_input=$item.frames
        dx11_direct_median_fps=$dx.steady_fps
        vulkan_wrapped_median_fps=$wrapped; vulkan_download_median_fps=$download
        dx11_4k60_pass=($item.width -ne 3840 -or $dx.steady_fps -ge 60)
        note='DX11はsteady direct。Vulkan wrappedはGPU readbackなし、downloadはGPU完了と転送を含む。fpsは同一測定境界ではない。'
    }
    $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
    Write-Host "$tag`: DX11=$([math]::Round($dx.steady_fps,1)) Vulkan-wrapped=$([math]::Round($wrapped,1)) Vulkan-download=$([math]::Round($download,1))"
}
if (@($records | Where-Object { !$_.dx11_4k60_pass }).Count) {
    throw 'M2新形式の4K60 directゲート未達。summary.jsonを参照'
}
