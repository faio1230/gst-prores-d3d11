# M1の新しい422プロファイルを固定SDKで4K/60fps・各60枚生成する。
param([string]$OutDir = 'results/profile-422-expansion-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$ffprobe = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffprobe.exe'
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
$records = @()
foreach ($profile in @(
    @{ Name='proxy'; Index=0; FourCC='apco' },
    @{ Name='lt'; Index=1; FourCC='apcs' },
    @{ Name='standard'; Index=2; FourCC='apcn' }
)) {
    $name = "profile-synthetic-$($profile.Name)-2160p60-rec709.mov"
    $file = Join-Path $root "media/$name"
    $log = Join-Path $out "generate-4k-$($profile.Name).log"
    if (-not (Test-Path -LiteralPath $file)) {
        & $ffmpeg -hide_banner -y -loglevel error -f lavfi -i 'testsrc2=size=3840x2160:rate=60' `
            -frames:v 60 -vf 'format=yuv422p10le,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709' `
            -c:v prores_ks -profile:v $profile.Index -alpha_bits 0 -tag:v $profile.FourCC `
            -movflags +write_colr $file *> $log
        if ($LASTEXITCODE) { throw "4K fixtureの生成に失敗: $name ($log)" }
    }
    $probe = (& $ffprobe -v error -select_streams v:0 -show_entries `
        'stream=codec_tag_string,pix_fmt,width,height,nb_frames,color_space,color_transfer,color_primaries' `
        -of json $file | ConvertFrom-Json).streams[0]
    if ($probe.codec_tag_string -ne $profile.FourCC -or $probe.pix_fmt -ne 'yuv422p10le' -or
        $probe.width -ne 3840 -or $probe.height -ne 2160 -or [int]$probe.nb_frames -ne 60 -or
        $probe.color_space -ne 'bt709' -or $probe.color_transfer -ne 'bt709' -or
        $probe.color_primaries -ne 'bt709') { throw "4K fixtureの属性が不一致: $name" }
    $records += [pscustomobject]@{
        input="media/$name"; origin='synthetic-4k'; profile=$profile.Name; fourcc=$profile.FourCC
        width=3840; height=2160; fps=60; frames=60
        sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    Write-Host "$name`: 60枚、SHA256確認"
}
$records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'fixtures-4k.json') -Encoding utf8
