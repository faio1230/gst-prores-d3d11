# 固定SDKだけでM4のTFF/BFF長尺・速度素材を生成する。
[CmdletBinding()]
param([string]$OutDir = 'build/m4-performance')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$sdk = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffmpeg = Join-Path $sdk 'ffmpeg.exe'
$ffprobe = Join-Path $sdk 'ffprobe.exe'
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($size in @('1080p30','4k60')) {
    $dimensions = if ($size -eq '1080p30') { '1920x1080' } else { '3840x2160' }
    $rate = if ($size -eq '1080p30') { 30 } else { 60 }
    $frames = if ($size -eq '1080p30') { 30 } else { 60 }
    foreach ($format in @('apch','ap4h-alpha16')) {
        $profile = if ($format -eq 'apch') { '3' } else { '4' }
        $pixels = if ($format -eq 'apch') { 'yuv422p10le' } else { 'yuva444p10le' }
        $alpha = if ($format -eq 'apch') { '0' } else { '16' }
        foreach ($order in @('tff','bff')) {
            $fieldOrder = if ($order -eq 'tff') { 'tt' } else { 'bb' }
            $name = "$format-$order-$size"
            $file = Join-Path $out "$name.mov"
            $log = Join-Path $out "$name-encode.log"
            if (!(Test-Path -LiteralPath $file)) {
                $filter = "format=$pixels,setfield=$order,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709"
                & $ffmpeg -hide_banner -y -loglevel error -f lavfi -i "testsrc2=size=$dimensions`:rate=$rate" `
                    -frames:v $frames -vf $filter -flags +ildct -field_order $fieldOrder `
                    -c:v prores_ks -profile:v $profile -alpha_bits $alpha -movflags +write_colr $file *> $log
                if ($LASTEXITCODE -ne 0) { throw "符号化失敗: $log" }
            }
            $info = (& $ffprobe -v error -select_streams v:0 `
                -show_entries stream=codec_tag_string,pix_fmt,width,height,nb_frames,field_order `
                -of json $file | ConvertFrom-Json).streams[0]
            $expectedProbeOrder = if ($order -eq 'tff') { 'tb' } else { 'bt' }
            if ($info.nb_frames -ne "$frames" -or $info.field_order -ne $expectedProbeOrder) {
                throw "素材検査失敗: $file"
            }
            Write-Output "$name sha256=$((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()) bytes=$((Get-Item -LiteralPath $file).Length)"
        }
    }
}
