# 量子化値が不変のProRes HQを固定SDKで生成し、IDCTジョブ再転送省略を検査する。
[CmdletBinding()]
param(
    [string]$OutDir = 'build/vs18/idct-cache-fixture',
    [string]$SummaryOut = 'results/idct-cache-fixture-2026-09-24.json'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = [IO.Path]::GetFullPath((Join-Path $root $OutDir))
$allowed = [IO.Path]::GetFullPath((Join-Path $root 'build/vs18')) + [IO.Path]::DirectorySeparatorChar
if (!$out.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $out)) {
    throw '出力先は未作成のbuild/vs18ディレクトリ内を指定してください'
}
$sdk = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffmpeg = Join-Path $sdk 'ffmpeg.exe'
$ffprobe = Join-Path $sdk 'ffprobe.exe'
foreach ($required in @($ffmpeg, $ffprobe)) {
    if (!(Test-Path -LiteralPath $required)) { throw "固定SDKがありません: $required" }
}
New-Item -ItemType Directory -Path $out | Out-Null
$source = Join-Path $out 'constant-hq.mov'
$tagged = Join-Path $out 'constant-bt709.mov'
& $ffmpeg -v error -xerror -f lavfi -i 'color=c=gray:s=1920x1080:r=60' `
    -frames:v 180 -vf 'format=yuv422p10le' -c:v prores_ks -profile:v 3 `
    -vendor apl0 -color_primaries bt709 -color_trc bt709 -colorspace bt709 -n $source
if ($LASTEXITCODE) { throw 'ProRes HQ生成に失敗しました' }
& $ffmpeg -v error -xerror -i $source -map 0:v:0 -c:v copy `
    -color_primaries bt709 -color_trc bt709 -colorspace bt709 -movflags +write_colr -n $tagged
if ($LASTEXITCODE) { throw 'BT.709タグのremuxに失敗しました' }
$stream = ((& $ffprobe -v error -select_streams v:0 -show_entries `
    stream=codec_name,profile,pix_fmt,width,height,color_space,color_transfer,color_primaries,nb_frames `
    -of json $tagged | ConvertFrom-Json).streams | Select-Object -First 1)
if ($LASTEXITCODE -or $stream.codec_name -ne 'prores' -or $stream.profile -ne 'HQ' -or
    $stream.pix_fmt -ne 'yuv422p10le' -or $stream.width -ne 1920 -or
    $stream.height -ne 1080 -or $stream.nb_frames -ne '180' -or
    $stream.color_space -ne 'bt709' -or $stream.color_transfer -ne 'bt709' -or
    $stream.color_primaries -ne 'bt709') { throw '生成素材の検査に失敗しました' }
$record = [ordered]@{
    status = '内部検証素材。MOV本体はGitに含めない'
    fixed_ffmpeg = $ffmpeg
    source = $source
    source_sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    tagged = $tagged
    tagged_sha256 = (Get-FileHash -LiteralPath $tagged -Algorithm SHA256).Hash.ToLowerInvariant()
    frames = 180
    format = 'ProRes 422 HQ 1920x1080/60 BT.709'
}
$summary = Join-Path $root $SummaryOut
New-Item -ItemType Directory -Force -Path (Split-Path $summary -Parent) | Out-Null
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summary -Encoding utf8
Write-Host "IDCTキャッシュ検証素材を生成: $tagged"
