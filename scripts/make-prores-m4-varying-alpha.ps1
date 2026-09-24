# 偶数・奇数行で値が変わるアルファを持つM4フィールド検査素材。
[CmdletBinding()]
param([string]$OutDir = 'build/m4-performance')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($order in @('tff','bff')) {
    $name = "ap4h-alpha16-varying-$order-320x180"
    $file = Join-Path $out "$name.mov"
    $fieldOrder = if ($order -eq 'tff') { 'tt' } else { 'bb' }
    $filter = "format=yuva444p10le,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='512+500*sin(Y/17+X/41)',setfield=$order,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709"
    & $ffmpeg -hide_banner -y -loglevel error -f lavfi -i 'testsrc2=size=320x180:rate=30' `
        -frames:v 2 -vf $filter -flags +ildct -field_order $fieldOrder `
        -c:v prores_ks -profile:v 4 -alpha_bits 16 -movflags +write_colr $file `
        *> (Join-Path $out "$name-encode.log")
    if ($LASTEXITCODE) { throw "可変アルファ素材の生成失敗: $name" }
    Write-Output "$name sha256=$((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant())"
}
