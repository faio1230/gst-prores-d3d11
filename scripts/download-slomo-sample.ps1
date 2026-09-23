# 公開4K HQサンプルをRange分割で取得する。利用先アプリには触れない。
[CmdletBinding()]
param([string]$Url = 'https://cloud.slomo.tv/s/6CnFDpArBb5KzRB/download')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$parts = Join-Path $root 'build/slomo-download-parts'
$destination = Join-Path $root 'media/reference-slomo-4k-hq60-complete.mov'
$total = 407239600L
$count = 8
New-Item -ItemType Directory -Force -Path $parts | Out-Null
$jobs = @()
$pending = @()
for ($index = 0; $index -lt $count; $index++) {
    $start = [long][math]::Floor($index * $total / $count)
    $end = [long][math]::Floor(($index + 1) * $total / $count) - 1
    $part = Join-Path $parts ('part-{0:D2}.bin' -f $index)
    $expected = $end - $start + 1
    if ((Test-Path -LiteralPath $part) -and (Get-Item -LiteralPath $part).Length -eq $expected) { continue }
    $segments = @()
    for ($segmentStart = $start; $segmentStart -le $end; $segmentStart += 1048576L) {
        $segmentEnd = [math]::Min($end, $segmentStart + 1048575L)
        $segment = Join-Path $parts ('part-{0:D2}-{1:D3}.bin' -f $index, $segments.Count)
        $segments += $segment
        if ((Test-Path -LiteralPath $segment) -and
            (Get-Item -LiteralPath $segment).Length -eq $segmentEnd - $segmentStart + 1) { continue }
        $jobs += Start-ThreadJob -ThrottleLimit 8 -ArgumentList $Url, $segmentStart, $segmentEnd, $segment -ScriptBlock {
            param($url, $start, $end, $segment)
            & curl.exe -fL --retry 4 --max-time 120 --silent --show-error --range "$start-$end" --output $segment $url
            if ($LASTEXITCODE) { throw "Range $start-$end 取得失敗: $LASTEXITCODE" }
            if ((Get-Item -LiteralPath $segment).Length -ne $end - $start + 1) {
                throw "Range $start-$end 長さ不一致"
            }
        }
    }
    $pending += [pscustomobject]@{ Part = $part; Segments = $segments; Expected = $expected }
}
if ($jobs) {
    $jobs | Wait-Job | Out-Null
    $jobs | Receive-Job -ErrorAction Continue
    if ($jobs | Where-Object State -ne Completed) { throw '分割取得に失敗' }
}
foreach ($item in $pending) {
    $assembled = "$($item.Part).complete"
    $stream = [System.IO.File]::Create($assembled)
    try {
        foreach ($segment in $item.Segments) {
            $inputStream = [System.IO.File]::OpenRead($segment)
            try { $inputStream.CopyTo($stream) } finally { $inputStream.Dispose() }
        }
    } finally { $stream.Dispose() }
    if ((Get-Item -LiteralPath $assembled).Length -ne $item.Expected) {
        throw "分割結合後の長さ不一致: $assembled"
    }
    Move-Item -LiteralPath $assembled -Destination $item.Part -Force
}
$stream = [System.IO.File]::Create($destination)
try {
    for ($index = 0; $index -lt $count; $index++) {
        $part = Join-Path $parts ('part-{0:D2}.bin' -f $index)
        $inputStream = [System.IO.File]::OpenRead($part)
        try { $inputStream.CopyTo($stream) } finally { $inputStream.Dispose() }
    }
} finally { $stream.Dispose() }
if ((Get-Item -LiteralPath $destination).Length -ne $total) { throw '結合後の長さが一致しません' }
$ffprobe = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffprobe.exe'
& $ffprobe -v error -show_entries stream=codec_name,profile,width,height,r_frame_rate,nb_frames -select_streams v:0 -of json $destination
if ($LASTEXITCODE) { throw '結合したMOVの検査に失敗' }
Get-FileHash -LiteralPath $destination -Algorithm SHA256 | Select-Object Path,Hash
