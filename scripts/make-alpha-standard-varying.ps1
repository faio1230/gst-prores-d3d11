# 標準AYUV64検査向け、非不透明alphaの独立合成素材を固定SDKで作る。
[CmdletBinding()]
param([string]$OutDir = 'build/alpha-standard')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$combiner = Join-Path $root 'build/vs18/Release/make_prores_422_alpha.exe'
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($bits in @(8, 16)) {
    $name = "ap4h-alpha$bits-varying"
    $file = Join-Path $out "$name.mov"
    $filter = if ($bits -eq 8) {
        "format=yuva444p,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='128+100*sin(Y/17+X/41)',setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709"
    } else {
        "format=yuva444p16le,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='32768+25000*sin(Y/17+X/41)',setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709"
    }
    & $ffmpeg -hide_banner -y -loglevel error -f lavfi -i 'testsrc2=size=320x180:rate=30' `
        -frames:v 2 -vf $filter -c:v prores_ks -profile:v 4 -alpha_bits $bits `
        -movflags +write_colr $file *> (Join-Path $out "$name-encode.log")
    if ($LASTEXITCODE) { throw "可変alphaの符号化失敗: $name" }
    Write-Output "$name sha256=$((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant())"
}
$source = Join-Path $out 'ap4h-alpha8-varying.mov'
$retag = Join-Path $out 'apch-alpha8-varying-444-retag.mov'
& $ffmpeg -hide_banner -y -loglevel error -i $source -map '0:v:0' -c:v copy -tag:v apch `
    $retag *> (Join-Path $out 'apch-alpha8-retag.log')
if ($LASTEXITCODE) { throw 'apchへのFourCC変更に失敗' }
$base = Join-Path $root 'media/feature-matrix-2026-09-24/apch.mov'
$combined = Join-Path $out 'apch-422-alpha8-varying-untagged.mov'
$savedPath = $env:PATH
try {
    $env:PATH = "$(Split-Path $ffmpeg -Parent);$savedPath"
    & $combiner $base $retag $combined *> (Join-Path $out 'apch-422-alpha8-combine.log')
    if ($LASTEXITCODE) { throw '422+alpha8の圧縮面合成に失敗' }
} finally { $env:PATH = $savedPath }
$output = Join-Path $out 'apch-422-alpha8-varying-bt709.mov'
& $ffmpeg -hide_banner -y -loglevel error -i $combined -map '0:v:0' -c:v copy `
    -color_primaries bt709 -color_trc bt709 -colorspace bt709 $output `
    *> (Join-Path $out 'apch-422-alpha8-color.log')
if ($LASTEXITCODE) { throw '422+alpha8の色タグ設定に失敗' }
Write-Output "apch-422-alpha8-varying-bt709 sha256=$((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant())"
