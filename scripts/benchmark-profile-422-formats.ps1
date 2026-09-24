# M1の新プロファイルを1080p/4Kでdirect測定し、同素材のFFmpeg Vulkanも記録する。
param([string]$OutDir = 'results/profile-422-format-performance-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$python = (Get-Command python -ErrorAction Stop).Source
$fixtures1080 = Get-Content -LiteralPath (Join-Path $root 'results/profile-422-expansion-2026-09-24/fixtures.json') -Raw | ConvertFrom-Json |
    Where-Object { $_.origin -eq 'synthetic' -and $_.profile -ne 'hq' }
$fixtures4k = Get-Content -LiteralPath (Join-Path $root 'results/profile-422-expansion-2026-09-24/fixtures-4k.json') -Raw | ConvertFrom-Json
$fixtures = @($fixtures1080) + @($fixtures4k)
if (Test-Path -LiteralPath $out) { throw "既存の結果を上書きしません: $out" }
if (-not (Test-Path -LiteralPath $ffmpeg) -or $fixtures.Count -ne 6) { throw '固定SDKまたは6素材が不足' }
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
foreach ($item in $fixtures) {
    $input = Join-Path $root $item.input
    if (-not (Test-Path -LiteralPath $input) -or
        (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
        throw "素材SHA256が不一致: $($item.input)"
    }
    $dimension = if ($item.origin -eq 'synthetic-4k') { '4k' } else { '1080p' }
    $tag = "$($item.profile)-$dimension"
    $trial = Join-Path $out "dx11-$tag"
    & $python (Join-Path $root 'scripts/benchmark-d3d11-plugin.py') $input `
        --modes dx11-direct --repeats 3 --loops 5 --warmup 0 `
        --frames-per-loop $item.frames --seeks 0 --startups 0 `
        --build-dir (Join-Path $root 'build/vs18') --out $trial `
        *> (Join-Path $out "dx11-$tag.log")
    if ($LASTEXITCODE) { throw "DX11 direct測定に失敗: $tag" }
    $dx = @(Get-Content -LiteralPath (Join-Path $trial 'medians.json') -Raw | ConvertFrom-Json)[0]
    if ($dx.mode -ne 'dx11-direct' -or $dx.steady_fps -le 0 -or $dx.gpu_completion_wait) {
        throw "DX11 direct条件が不正: $tag"
    }
    $vkResults = @()
    foreach ($method in @('direct','download')) {
        for ($repeat = 0; $repeat -lt 3; ++$repeat) {
            $logPath = Join-Path $out "vulkan-$tag-$method-r$repeat.log"
            $expected = $item.frames * 5
            $args = @('-hide_banner','-nostats','-loglevel','verbose','-benchmark',
                '-init_hw_device','vulkan=vk:0','-filter_hw_device','vk',
                '-hwaccel','vulkan','-hwaccel_output_format','vulkan',
                '-stream_loop','4','-i',$input,'-frames:v',[string]$expected)
            if ($method -eq 'direct') {
                $args += @('-noautoscale','-c:v','wrapped_avframe')
            } else {
                $args += @('-vf','hwdownload,format=yuv422p10le')
            }
            $args += @('-f','null','NUL')
            & $ffmpeg @args *> $logPath
            $code = $LASTEXITCODE
            $log = Get-Content -LiteralPath $logPath -Raw
            $encoded = [regex]::Matches($log, '(\d+) frames encoded')
            $count = if ($encoded.Count) { [int]$encoded[$encoded.Count-1].Groups[1].Value } else { 0 }
            $timing = [regex]::Matches($log, 'bench: utime=[0-9.]+s stime=[0-9.]+s rtime=([0-9.]+)s')
            $seconds = if ($timing.Count) { [double]::Parse($timing[$timing.Count-1].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) } else { 0 }
            if ($code -ne 0 -or $log -notmatch 'Device 0 selected: NVIDIA GeForce RTX 3070' -or
                $log -notmatch 'pixfmt:vulkan' -or $count -ne $expected -or $seconds -le 0) {
                throw "Vulkan測定条件が不正: $tag/$method/$repeat ($logPath)"
            }
            $vkResults += [pscustomobject]@{ method=$method; repeat=$repeat; frames=$count; seconds=$seconds; fps=$count/$seconds }
        }
    }
    $direct = @($vkResults | Where-Object method -EQ 'direct' | ForEach-Object fps | Sort-Object)[1]
    $download = @($vkResults | Where-Object method -EQ 'download' | ForEach-Object fps | Sort-Object)[1]
    $row = [pscustomobject]@{
        input=$item.input; sha256=$item.sha256; profile=$item.profile; resolution=$dimension
        frames_per_input=$item.frames; dx11_direct_median_fps=$dx.steady_fps
        vulkan_wrapped_median_fps=$direct; vulkan_download_median_fps=$download
        dx11_4k60_pass=($dimension -ne '4k' -or $dx.steady_fps -ge 60)
        note='DX11はsteady direct。Vulkan wrappedはGPU画像readbackなし、downloadはGPU完了+転送を含む。fps同士は同一測定境界ではない。'
    }
    $records += $row
    $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
    Write-Host "$tag`: DX11=$([math]::Round($dx.steady_fps,1)) Vulkan-wrapped=$([math]::Round($direct,1)) Vulkan-download=$([math]::Round($download,1))"
}
if (@($records | Where-Object { -not $_.dx11_4k60_pass }).Count) { throw '新形式の4K60 directゲート未達' }
