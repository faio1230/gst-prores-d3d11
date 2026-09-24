# M4長尺素材を固定SDKのVulkan hwaccelで全フレームGPU復号・読戻しする。
[CmdletBinding()]
param([string]$OutDir = 'results/m4-vulkan-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sdk = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffmpeg = Join-Path $sdk 'ffmpeg.exe'
$out = Join-Path $root $OutDir
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
foreach ($size in @('1080p30','4k60')) {
    $frames = if ($size -eq '1080p30') { 30 } else { 60 }
    foreach ($format in @('apch','ap4h-alpha16')) {
        $pixels = if ($format -eq 'apch') { 'yuv422p10le' } else { 'yuva444p12le' }
        foreach ($order in @('tff','bff')) {
            $name = "$format-$order-$size"
            $input = Join-Path $root "build/m4-performance/$name.mov"
            $log = Join-Path $out "$name.log"
            $md5 = Join-Path $out "$name.framemd5"
            & $ffmpeg -hide_banner -y -loglevel verbose -benchmark `
                -init_hw_device vulkan=vk:0 -filter_hw_device vk `
                -hwaccel vulkan -hwaccel_output_format vulkan -i $input `
                -map '0:v:0' -frames:v $frames -vf "hwdownload,format=$pixels" `
                -f framemd5 $md5 *> $log
            $exit = $LASTEXITCODE
            $content = Get-Content -LiteralPath $log -Raw
            $count = @(Get-Content -LiteralPath $md5 | Where-Object { $_ -match '^\d+,' }).Count
            $gpu = $content -match 'pixfmt:vulkan' -and
                $content -match 'Device 0 selected: NVIDIA GeForce RTX 3070'
            if ($exit -ne 0 -or !$gpu -or $count -ne $frames) {
                throw "Vulkan GPU検査失敗: $name ($log)"
            }
            $records += [pscustomobject]@{
                input="build/m4-performance/$name.mov"
                sha256=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant()
                frames=$count; gpu=$gpu; download_pix_fmt=$pixels; passed=$true
            }
            $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
            Write-Output "$name Vulkan GPU $count/$frames"
        }
    }
}
