# 422系4プロファイルを固定FFmpeg SDKで生成する。実写由来も元の撮影profileではなく再エンコード素材。
[CmdletBinding()]
param(
    [string]$OutDir = 'results/profile-422-expansion-2026-09-24'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$ffprobe = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffprobe.exe'
$cameraSource = Join-Path $root 'media/reference-proton-rec709-hq.mov'
$cameraHash = if (Test-Path -LiteralPath $cameraSource) {
    (Get-FileHash -LiteralPath $cameraSource -Algorithm SHA256).Hash.ToLowerInvariant()
} else { $null }
$out = Join-Path $root $OutDir
foreach ($required in @($ffmpeg, $ffprobe, $cameraSource)) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要な固定依存物がありません: $required" }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$profiles = @(
    @{ Name = 'proxy'; Index = 0; Tag = 'apco' },
    @{ Name = 'lt'; Index = 1; Tag = 'apcs' },
    @{ Name = 'standard'; Index = 2; Tag = 'apcn' },
    @{ Name = 'hq'; Index = 3; Tag = 'apch' }
)
$records = @()
foreach ($profile in $profiles) {
    foreach ($origin in @('synthetic', 'camera-transcode')) {
        $encodedName = "profile-$origin-$($profile.Name)-1080p30.mov"
        $encoded = Join-Path $root "media/$encodedName"
        $name = "profile-$origin-$($profile.Name)-1080p30-rec709.mov"
        $destination = Join-Path $root "media/$name"
        $log = Join-Path $out "generate-$origin-$($profile.Name).log"
        if (!(Test-Path -LiteralPath $encoded)) {
            $arguments = @('-hide_banner', '-loglevel', 'error', '-y')
            if ($origin -eq 'synthetic') {
                $arguments += @('-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=60')
            } else {
                $arguments += @('-i', $cameraSource, '-map', '0:v:0')
            }
            $arguments += @('-frames:v', '30', '-vf', 'format=yuv422p10le',
                '-c:v', 'prores_ks', '-profile:v', [string]$profile.Index,
                '-pix_fmt', 'yuv422p10le', '-tag:v', $profile.Tag,
                '-color_primaries', 'bt709', '-color_trc', 'bt709',
                '-colorspace', 'bt709', '-color_range', 'tv', $encoded)
            & $ffmpeg @arguments *> $log
            if ($LASTEXITCODE) { throw "固定FFmpegのエンコードに失敗: $encodedName ($log)" }
        }
        if (!(Test-Path -LiteralPath $destination)) {
            & $ffmpeg -hide_banner -loglevel error -y -i $encoded -map '0:v:0' -c:v copy `
                -color_primaries bt709 -color_trc bt709 -colorspace bt709 `
                -movflags +write_colr $destination *> (Join-Path $out "tag-$origin-$($profile.Name).log")
            if ($LASTEXITCODE) { throw "BT.709 MOVタグ付けに失敗: $name" }
        }
        $probe = (& $ffprobe -v error -select_streams v:0 -show_entries `
            stream=codec_name,profile,codec_tag_string,pix_fmt,width,height,nb_frames,color_space,color_transfer,color_primaries `
            -of json $destination | ConvertFrom-Json).streams[0]
        if ($LASTEXITCODE -or $probe.codec_name -ne 'prores' -or
            $probe.codec_tag_string -ne $profile.Tag -or
            $probe.profile.ToLowerInvariant() -ne $profile.Name -or
            $probe.pix_fmt -ne 'yuv422p10le' -or $probe.width -ne 1920 -or
            $probe.height -ne 1080 -or [int]$probe.nb_frames -ne 30 -or
            $probe.color_space -ne 'bt709' -or $probe.color_transfer -ne 'bt709' -or
            $probe.color_primaries -ne 'bt709') {
            throw "生成素材のprofile/format/枚数が不一致: $name"
        }
        $records += [pscustomobject]@{
            input = "media/$name"
            origin = $origin
            origin_note = if ($origin -eq 'camera-transcode') {
                '公開カメラ素材から固定FFmpegで再エンコード。撮影時のprofileではない'
            } else { 'lavfi testsrc2を固定FFmpegでエンコード' }
            origin_url = if ($origin -eq 'camera-transcode') {
                'https://doc.proton-camera.com/docs/samples/'
            } else { 'FFmpeg lavfi testsrc2' }
            origin_sha256 = if ($origin -eq 'camera-transcode') { $cameraHash } else { $null }
            license_note = if ($origin -eq 'camera-transcode') {
                '公式ページに視聴・ダウンロード案内あり。再配布・派生物ライセンスの明記なし。内部検査のみ、素材はGitに含めない。'
            } else { '自作合成素材。外部映像なし。' }
            profile = $profile.Name
            fourcc = $profile.Tag
            frames = 30
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        Write-Host "$name`: $($profile.Tag)、30枚"
    }
}
$records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'fixtures.json') -Encoding utf8
