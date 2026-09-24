# M3アルファ素材を固定SDKのVulkan GPU経路で測る。DX11 directとは境界が異なる。
[CmdletBinding()]
param(
    [string]$ValidationSummary = 'results/m3-alpha-1080-validation-2026-09-24/summary.json',
    [string]$OutDir = 'results/m3-alpha-1080-vulkan-2026-09-24'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$validation = Get-Content -LiteralPath (Join-Path $root $ValidationSummary) -Raw | ConvertFrom-Json
if (!$validation.fixtures -or (Test-Path -LiteralPath $out) -or !(Test-Path -LiteralPath $ffmpeg)) {
    throw '検証素材・固定SDKが不足、または結果が既にあります'
}
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
foreach ($item in $validation.fixtures) {
    $input = Join-Path $root $item.file
    if (!(Test-Path -LiteralPath $input) -or
        (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
        throw "素材SHA256が不一致: $($item.file)"
    }
    $tag = [IO.Path]::GetFileNameWithoutExtension($input)
    $expected = [int]$item.frames * 5
    $rows = @()
    foreach ($method in @('wrapped','download')) {
        for ($repeat = 0; $repeat -lt 3; ++$repeat) {
            $logPath = Join-Path $out "vulkan-$tag-$method-r$repeat.log"
            $arguments = @('-hide_banner','-nostats','-loglevel','verbose','-benchmark',
                '-init_hw_device','vulkan=vk:0','-filter_hw_device','vk',
                '-hwaccel','vulkan','-hwaccel_output_format','vulkan',
                '-stream_loop','4','-i',$input,'-frames:v',[string]$expected)
            if ($method -eq 'wrapped') {
                $arguments += @('-noautoscale','-c:v','wrapped_avframe')
            } else {
                $arguments += @('-vf',"hwdownload,format=$($item.pixel_format)")
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
                throw "Vulkan GPU測定条件が不正: $tag/$method/$repeat ($logPath)"
            }
            $rows += [pscustomobject]@{
                method=$method; repeat=$repeat; frames=$count; seconds=$seconds; fps=$count/$seconds
            }
        }
    }
    $records += [pscustomobject]@{
        input=$item.file; sha256=$item.sha256; fourcc=$item.fourcc
        pixel_format=$item.pixel_format; frames_per_input=$item.frames
        vulkan_wrapped_median_fps=@($rows | Where-Object method -EQ 'wrapped' |
            ForEach-Object fps | Sort-Object)[1]
        vulkan_download_median_fps=@($rows | Where-Object method -EQ 'download' |
            ForEach-Object fps | Sort-Object)[1]
        trials=$rows
        note='DX11 directとは測定境界が異なる。wrappedはGPU readbackなし、downloadはGPU完了と転送を含む。'
    }
    $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
    Write-Host "$tag`: Vulkan GPU測定完了"
}
