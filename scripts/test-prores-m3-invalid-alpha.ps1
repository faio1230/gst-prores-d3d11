# alpha_info=3の有効コンテナ／無効ProRes frameを固定SDKで拒否確認する。
[CmdletBinding()]
param([string]$OutDir = 'results/m3-alpha-invalid-mode-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $root 'media/feature-matrix-2026-09-24/ap4h-alpha8.mov'
$ffmpegBin = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffmpeg = Join-Path $ffmpegBin 'ffmpeg.exe'
$ffprobe = Join-Path $ffmpegBin 'ffprobe.exe'
$out = Join-Path $root $OutDir
if ((Test-Path -LiteralPath $out) -or !(Test-Path -LiteralPath $source)) {
    throw '既存結果があるか、元素材がありません'
}
New-Item -ItemType Directory -Path $out | Out-Null
$packetInfo = & $ffprobe -v error -select_streams v:0 -show_entries packet=pos,size -of json $source |
    ConvertFrom-Json
if (!$packetInfo.packets -or [int]$packetInfo.packets[0].size -le 25) {
    throw 'ProRes packetの位置を確認できません'
}
$offset = [int]$packetInfo.packets[0].pos + 25
$bytes = [IO.File]::ReadAllBytes($source)
if ($offset -ge $bytes.Length -or
    [Text.Encoding]::ASCII.GetString($bytes, [int]$packetInfo.packets[0].pos + 4, 4) -ne 'icpf' -or
    ($bytes[$offset] -band 15) -notin @(1,2)) {
    throw '元packetのProRes署名またはalpha modeが予想外です'
}
$bytes[$offset] = [byte](($bytes[$offset] -band 240) -bor 3)
$mutated = Join-Path $out 'invalid-alpha-mode3.mov'
[IO.File]::WriteAllBytes($mutated, $bytes)
$cases = @()
foreach ($method in @('cpu','vulkan')) {
    $log = Join-Path $out "$method.log"
    $arguments = @('-hide_banner','-loglevel','verbose','-xerror')
    if ($method -eq 'vulkan') {
        $arguments += @('-init_hw_device','vulkan=vk:0','-filter_hw_device','vk',
            '-hwaccel','vulkan','-hwaccel_output_format','vulkan')
    }
    $arguments += @('-i',$mutated,'-map','0:v:0','-frames:v','2','-f','null','NUL')
    & $ffmpeg @arguments *> $log
    $code = $LASTEXITCODE
    if ($code -eq 0) { throw "固定SDKが無効alpha modeを受理しました: $method" }
    $cases += [pscustomobject]@{ method=$method; exit_code=$code; rejected=$true }
}
[pscustomobject]@{
    source='media/feature-matrix-2026-09-24/ap4h-alpha8.mov'
    source_sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    mutation='先頭ProRes frameのalpha_infoだけを3へ変更'
    packet_offset=[int]$packetInfo.packets[0].pos
    mutated_sha256=(Get-FileHash -LiteralPath $mutated -Algorithm SHA256).Hash.ToLowerInvariant()
    cases=$cases
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
