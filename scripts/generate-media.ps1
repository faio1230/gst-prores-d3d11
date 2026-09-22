[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$ff=Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
New-Item -ItemType Directory -Force (Join-Path $root 'media'),(Join-Path $root 'results') | Out-Null
foreach ($item in @(@{Size='1920x1080';Name='synthetic-1080p60-hq'},@{Size='3840x2160';Name='synthetic-2160p60-hq'})) {
    & $ff -hide_banner -y -f lavfi -i "testsrc2=size=$($item.Size):rate=60" -frames:v 180 -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv (Join-Path $root "media/$($item.Name).mov") 2> (Join-Path $root "results/generate-$($item.Name).log")
    if ($LASTEXITCODE) { throw '合成422素材の生成に失敗' }
}
& $ff -hide_banner -y -f lavfi -i "testsrc2=size=1920x1080:rate=60,format=yuva444p10le,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a=1023*X/W" -frames:v 60 -c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le -alpha_bits 16 -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv (Join-Path $root 'media/synthetic-1080p60-4444-alpha.mov') 2> (Join-Path $root 'results/generate-4444.log')
if ($LASTEXITCODE) { throw '合成4444素材の生成に失敗' }
Get-ChildItem (Join-Path $root 'media') -Filter synthetic-*.mov | Get-FileHash -Algorithm SHA256 | ConvertTo-Json | Set-Content (Join-Path $root 'results/media-hashes.json') -Encoding utf8
