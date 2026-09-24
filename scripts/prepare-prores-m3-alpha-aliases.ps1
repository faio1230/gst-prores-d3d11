# 同一ProRes packetをFourCCだけ再タグし、M3の残るプロファイル交差条件を測る。
[CmdletBinding()]
param([string]$OutDir = 'results/m3-alpha-aliases-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$media = Join-Path $root 'build/m3-performance'
$out = Join-Path $root $OutDir
$ffmpegBin = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffmpeg = Join-Path $ffmpegBin 'ffmpeg.exe'
$ffprobe = Join-Path $ffmpegBin 'ffprobe.exe'
if ((Test-Path -LiteralPath $out) -or !(Test-Path -LiteralPath $ffmpeg) -or
    !(Test-Path -LiteralPath $ffprobe)) {
    throw '出力先が既にあるか、固定SDKがありません'
}
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
foreach ($resolution in @('1080p30','4k60')) {
    $width = if ($resolution -eq '1080p30') { 1920 } else { 3840 }
    $height = if ($resolution -eq '1080p30') { 1080 } else { 2160 }
    $frames = if ($resolution -eq '1080p30') { 30 } else { 60 }
    foreach ($chroma in @('422','444')) {
        foreach ($alphaBits in @(8,16)) {
            foreach ($mapping in @(
                @{ Source='ap4h'; Target='ap4x'; Depth=12 },
                @{ Source='apch'; Target='apco'; Depth=10 },
                @{ Source='apch'; Target='apcs'; Depth=10 },
                @{ Source='apch'; Target='apcn'; Depth=10 })) {
                $sourceName = "$($mapping.Source)-$chroma-alpha$alphaBits-$resolution.mov"
                $targetName = "$($mapping.Target)-$chroma-alpha$alphaBits-$resolution.mov"
                $source = Join-Path $media $sourceName
                $target = Join-Path $media $targetName
                if (!(Test-Path -LiteralPath $source) -or (Test-Path -LiteralPath $target)) {
                    throw "入力不足または既存出力: $sourceName → $targetName"
                }
                & $ffmpeg -hide_banner -loglevel error -n -i $source -map 0:v:0 `
                    -c:v copy -tag:v $mapping.Target $target
                if ($LASTEXITCODE) { throw "再タグ失敗: $targetName" }
                $stream = @(& $ffprobe -v error -select_streams v:0 `
                    -show_entries stream=codec_tag_string,pix_fmt,width,height,nb_frames `
                    -of json $target | ConvertFrom-Json).streams[0]
                $expectedFormat = "yuva${chroma}p$($mapping.Depth)le"
                if ($stream.codec_tag_string -ne $mapping.Target -or
                    $stream.pix_fmt -ne $expectedFormat -or
                    [int]$stream.width -ne $width -or [int]$stream.height -ne $height -or
                    [int]$stream.nb_frames -ne $frames) {
                    throw "再タグ形式不一致: $targetName"
                }
                $records += [pscustomobject]@{
                    file="build/m3-performance/$targetName"
                    sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
                    source="build/m3-performance/$sourceName"
                    source_sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
                    fourcc=$mapping.Target; chroma=$chroma; alpha_bits=$alphaBits
                    bit_depth=$mapping.Depth; pixel_format=$expectedFormat
                    width=$width; height=$height; frames=$frames
                    origin='同一圧縮packetのFourCC再タグ。独立エンコードや実写素材ではない。'
                }
                Write-Host "$targetName`: packet再タグ・形式確認完了"
            }
        }
    }
}
if ($records.Count -ne 32) { throw "素材数が不正: $($records.Count)" }
[pscustomobject]@{ fixtures=$records; files=$records.Count } | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $out 'manifest.json') -Encoding utf8
