# M2の8交差条件と公開カメラ素材由来の2条件を固定FFmpeg SDKで生成する。
[CmdletBinding()]
param(
    [string]$OutDir = 'results/prores-m2-2026-09-24',
    [switch]$Include4k,
    [switch]$IncludeRgbTags
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$ffprobe = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffprobe.exe'
$camera = Join-Path $root 'media/reference-proton-rec709-hq.mov'
$out = Join-Path $root $OutDir
$media = Join-Path $root 'media/prores-m2-2026-09-24'
foreach ($path in @($ffmpeg, $ffprobe, $camera)) {
    if (!(Test-Path -LiteralPath $path)) { throw "固定依存物がありません: $path" }
}
New-Item -ItemType Directory -Force -Path $out, $media | Out-Null
$cameraHash = (Get-FileHash -LiteralPath $camera -Algorithm SHA256).Hash.ToLowerInvariant()
$cases = @(
    @{ Name='apco-44410'; Tag='apco'; Profile=0; Chroma='444'; Depth=10 },
    @{ Name='apcs-44410'; Tag='apcs'; Profile=1; Chroma='444'; Depth=10 },
    @{ Name='apcn-44410'; Tag='apcn'; Profile=2; Chroma='444'; Depth=10 },
    @{ Name='apch-44410'; Tag='apch'; Profile=3; Chroma='444'; Depth=10 },
    @{ Name='ap4h-42212'; Tag='ap4h'; Profile=4; Chroma='422'; Depth=12 },
    @{ Name='ap4x-42212'; Tag='ap4x'; Profile=5; Chroma='422'; Depth=12 },
    @{ Name='ap4h-44412'; Tag='ap4h'; Profile=4; Chroma='444'; Depth=12 },
    @{ Name='ap4x-44412'; Tag='ap4x'; Profile=5; Chroma='444'; Depth=12 }
)
$records = @()
foreach ($case in $cases) {
    $origins = if ($case.Name -in @('apch-44410', 'ap4h-44412')) {
        @('synthetic', 'camera-transcode')
    } else { @('synthetic') }
    foreach ($origin in $origins) {
        $name = "$($case.Name)-$origin-1080p30.mov"
        $destination = Join-Path $media $name
        $log = Join-Path $out "generate-$name.log"
        if (!(Test-Path -LiteralPath $destination)) {
            $source = if ($origin -eq 'synthetic') {
                @('-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=30')
            } else { @('-i', $camera, '-map', '0:v:0') }
            $arguments = @('-hide_banner', '-y', '-loglevel', 'error') + $source + @(
                '-frames:v', '30', '-vf', "format=yuv$($case.Chroma)p10le",
                '-c:v', 'prores_ks', '-profile:v', [string]$case.Profile,
                '-alpha_bits', '0', '-tag:v', $case.Tag,
                '-color_primaries', 'bt709', '-color_trc', 'bt709',
                '-colorspace', 'bt709', '-color_range', 'tv', '-movflags', '+write_colr',
                $destination)
            & $ffmpeg @arguments *> $log
            if ($LASTEXITCODE) { throw "生成失敗: $name ($log)" }
        }
        $probe = (& $ffprobe -v error -select_streams v:0 -show_entries `
            stream=codec_name,codec_tag_string,pix_fmt,width,height,nb_frames -of json $destination |
            ConvertFrom-Json).streams[0]
        $expectedFormat = "yuv$($case.Chroma)p$($case.Depth)le"
        if ($LASTEXITCODE -or $probe.codec_name -ne 'prores' -or
            $probe.codec_tag_string -ne $case.Tag -or $probe.pix_fmt -ne $expectedFormat -or
            $probe.width -ne 1920 -or $probe.height -ne 1080 -or [int]$probe.nb_frames -ne 30) {
            throw "形式・枚数が不一致: $name"
        }
        $records += [pscustomobject]@{
            input = "media/prores-m2-2026-09-24/$name"
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            fourcc = $case.Tag; chroma = $case.Chroma; bit_depth = $case.Depth
            origin = $origin; frames = 30; width = 1920; height = 1080
            origin_url = if ($origin -eq 'camera-transcode') {
                'https://doc.proton-camera.com/docs/samples/'
            } else { 'FFmpeg lavfi testsrc2' }
            origin_sha256 = if ($origin -eq 'camera-transcode') { $cameraHash } else { $null }
            license_note = if ($origin -eq 'camera-transcode') {
                '公式ページにダウンロード案内あり。再配布・派生物ライセンスは未確認。内部検査のみ、素材はGitへ含めない。'
            } else { '自作合成素材。外部映像なし。' }
        }
        Write-Host "$name`: $($case.Tag)、$expectedFormat、30枚"
    }
}
$records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'fixtures.json') -Encoding utf8
if ($Include4k) {
    $records4k = @()
    foreach ($case in $cases) {
        $name = "$($case.Name)-synthetic-4k60.mov"
        $destination = Join-Path $media $name
        $log = Join-Path $out "generate-$name.log"
        if (!(Test-Path -LiteralPath $destination)) {
            if ((Get-PSDrive C).Free -lt 2GB) { throw '4K素材用の空き容量が2GiB未満' }
            & $ffmpeg -hide_banner -y -loglevel error -f lavfi `
                -i 'testsrc2=size=3840x2160:rate=60' -frames:v 60 `
                -vf "format=yuv$($case.Chroma)p10le" -c:v prores_ks `
                -profile:v $case.Profile -alpha_bits 0 -tag:v $case.Tag `
                -color_primaries bt709 -color_trc bt709 -colorspace bt709 `
                -color_range tv -movflags +write_colr $destination *> $log
            if ($LASTEXITCODE) { throw "4K生成失敗: $name ($log)" }
        }
        $probe = (& $ffprobe -v error -select_streams v:0 -show_entries `
            stream=codec_name,codec_tag_string,pix_fmt,width,height,nb_frames -of json $destination |
            ConvertFrom-Json).streams[0]
        $expectedFormat = "yuv$($case.Chroma)p$($case.Depth)le"
        if ($LASTEXITCODE -or $probe.codec_name -ne 'prores' -or
            $probe.codec_tag_string -ne $case.Tag -or $probe.pix_fmt -ne $expectedFormat -or
            $probe.width -ne 3840 -or $probe.height -ne 2160 -or [int]$probe.nb_frames -ne 60) {
            throw "4K形式・枚数が不一致: $name"
        }
        $records4k += [pscustomobject]@{
            input = "media/prores-m2-2026-09-24/$name"
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            fourcc = $case.Tag; chroma = $case.Chroma; bit_depth = $case.Depth
            origin = 'synthetic-4k'; frames = 60; width = 3840; height = 2160
            origin_url = 'FFmpeg lavfi testsrc2'; origin_sha256 = $null
            license_note = '自作合成素材。外部映像なし。'
        }
        Write-Host "$name`: $($case.Tag)、$expectedFormat、60枚"
    }
    $records4k | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'fixtures-4k.json') -Encoding utf8
}
if ($IncludeRgbTags) {
    $all = @(Get-Content -LiteralPath (Join-Path $out 'fixtures.json') -Raw | ConvertFrom-Json) +
        @(Get-Content -LiteralPath (Join-Path $out 'fixtures-4k.json') -Raw | ConvertFrom-Json)
    if ($all.Count -ne 18) { throw 'RGBタグ付けには1080p/4Kの18素材が必要' }
    $rgbRecords = @()
    foreach ($item in $all) {
        $original = Join-Path $root $item.input
        if ((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
            throw "元素材SHA256が不一致: $($item.input)"
        }
        $tagged = Join-Path ([IO.Path]::GetDirectoryName($original)) `
            ([IO.Path]::GetFileNameWithoutExtension($original) + '-rec709.mov')
        $name = [IO.Path]::GetFileName($tagged)
        if (!(Test-Path -LiteralPath $tagged)) {
            & $ffmpeg -hide_banner -y -loglevel error -i $original -map '0:v:0' -c:v copy `
                -color_primaries bt709 -color_trc bt709 -colorspace bt709 `
                -movflags +write_colr $tagged *> (Join-Path $out "tag-$name.log")
            if ($LASTEXITCODE) { throw "BT.709タグ付け失敗: $name" }
        }
        $probe = (& $ffprobe -v error -select_streams v:0 -show_entries `
            stream=codec_tag_string,pix_fmt,nb_frames,color_space,color_transfer,color_primaries `
            -of json $tagged | ConvertFrom-Json).streams[0]
        if ($LASTEXITCODE -or $probe.codec_tag_string -ne $item.fourcc -or
            $probe.pix_fmt -ne "yuv$($item.chroma)p$($item.bit_depth)le" -or
            [int]$probe.nb_frames -ne $item.frames -or $probe.color_space -ne 'bt709' -or
            $probe.color_transfer -ne 'bt709' -or $probe.color_primaries -ne 'bt709') {
            throw "RGB用タグ付き素材の形式が不正: $name"
        }
        $rgbRecords += [pscustomobject]@{
            input = "media/prores-m2-2026-09-24/$name"
            sha256 = (Get-FileHash -LiteralPath $tagged -Algorithm SHA256).Hash.ToLowerInvariant()
            source_input = $item.input; source_sha256 = $item.sha256
            fourcc = $item.fourcc; chroma = $item.chroma; bit_depth = $item.bit_depth
            origin = $item.origin; frames = $item.frames; width = $item.width; height = $item.height
            note = '元MOVを再符号化せずcopyし、BT.709 MOV色タグを付けたRGB検証用素材。'
        }
        Write-Host "$name`: BT.709タグ確認、$($item.frames)枚"
    }
    $rgbRecords | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'fixtures-rgb.json') -Encoding utf8
}
