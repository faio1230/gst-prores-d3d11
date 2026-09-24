# FFmpegがProRes frame_type=3をGPUで受理するか、BFF素材のheaderだけを変えて確認する。
[CmdletBinding()]
param([string]$OutDir = 'build/m4-fieldtype3')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $root 'media/feature-matrix-2026-09-24/apch-bff.mov'
$sdk = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffmpeg = Join-Path $sdk 'ffmpeg.exe'
$ffprobe = Join-Path $sdk 'ffprobe.exe'
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
$packets = (& $ffprobe -v error -select_streams v:0 -show_entries packet=pos,size -of json $source |
    ConvertFrom-Json).packets
if ($packets.Count -ne 2) { throw '2フレームの元素材が必要です' }
$bytes = [IO.File]::ReadAllBytes($source)
foreach ($packet in $packets) {
    $offset = [int]$packet.pos + 20
    if ([Text.Encoding]::ASCII.GetString($bytes, [int]$packet.pos + 4, 4) -ne 'icpf' -or
        (($bytes[$offset] -shr 2) -band 3) -ne 2) { throw 'BFF packetの位置またはheaderが予想外です' }
    $bytes[$offset] = [byte]($bytes[$offset] -bor 4)
}
$mutated = Join-Path $out 'apch-fieldtype3.mov'
[IO.File]::WriteAllBytes($mutated, $bytes)
foreach ($method in @('cpu','vulkan')) {
    $arguments = @('-hide_banner','-loglevel','verbose','-xerror')
    if ($method -eq 'vulkan') {
        $arguments += @('-init_hw_device','vulkan=vk:0','-filter_hw_device','vk',
            '-hwaccel','vulkan','-hwaccel_output_format','vulkan')
    }
    $arguments += @('-i',$mutated,'-map','0:v:0','-frames:v','2','-f','null','NUL')
    & $ffmpeg @arguments *> (Join-Path $out "$method.log")
    $log = Get-Content (Join-Path $out "$method.log") -Raw
    Write-Output "$method exit=$LASTEXITCODE gpu=$($log -match 'pixfmt:vulkan')"
}
