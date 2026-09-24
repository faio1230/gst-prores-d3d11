# M4の全フレームで製品VLD CSOの係数とGPU IDCT画素をCPU参照と照合する。
[CmdletBinding()]
param([string]$OutDir = 'results/m4-long-coefficients-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$validator = Join-Path $root 'build/vs18/Release/prores_dx11_coeff.exe'
$shader = Join-Path $root 'build/vs18/plugins/Release/prores_vld.cso'
$sdk = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$ffprobe = Join-Path $sdk 'ffprobe.exe'
$out = Join-Path $root $OutDir
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$inputs = @()
foreach ($size in @('1080p30','4k60')) {
    foreach ($name in @('apch-tff','apch-bff','ap4h-alpha16-tff','ap4h-alpha16-bff')) {
        $inputs += Join-Path $root "build/m4-performance/$name-$size.mov"
    }
}
$inputs += Join-Path $root 'build/m4-fieldtype3/apch-fieldtype3.mov'
$savedPath = $env:PATH
$records = @()
try {
    $env:PATH = "$sdk;$savedPath"
    foreach ($input in $inputs) {
        $tag = [IO.Path]::GetFileNameWithoutExtension($input)
        $info = (& $ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames -of json $input |
            ConvertFrom-Json).streams[0]
        $frames = [int]$info.nb_frames
        if ($frames -le 0) { throw "フレーム数不明: $input" }
        $log = Join-Path $out "$tag.jsonl"
        $coefficientCount = [long]0
        $pixelMax = 0
        for ($i = 0; $i -lt $frames; ++$i) {
            $lines = @(& $validator $input $shader "--frame=$i" 2>&1 | ForEach-Object { "$_" })
            $lines | Add-Content -LiteralPath $log -Encoding utf8
            if ($LASTEXITCODE) { throw "係数・画素検査失敗: $tag frame=$i ($log)" }
            $row = $lines | Select-Object -Last 1 | ConvertFrom-Json
            $frameMax = ($row.pixel_differences | Measure-Object max_abs -Maximum).Maximum
            if (!$row.passed -or $row.mismatches -ne 0 -or $row.shader_errors -ne 0 -or
                $frameMax -gt 1) { throw "検査値不一致: $tag frame=$i ($log)" }
            $coefficientCount += [long]$row.coefficients
            $pixelMax = [math]::Max($pixelMax, $frameMax)
        }
        $records += [pscustomobject]@{
            input=$input.Substring($root.Length + 1).Replace('\','/')
            sha256=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant()
            frames=$frames; coefficients_checked=$coefficientCount
            coefficient_mismatches=0; shader_errors=0; pixel_max_abs=$pixelMax; passed=$true
        }
        $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
        Write-Output "$tag frames=$frames coeff=$coefficientCount pixel_max=$pixelMax"
    }
} finally {
    $env:PATH = $savedPath
}
