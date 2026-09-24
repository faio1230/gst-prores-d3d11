# M4のTFF/BFF・アルファ有無を1080p/4KのDX11 direct境界で測る。
[CmdletBinding()]
param([string]$OutDir = 'results/m4-direct-performance-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
foreach ($size in @('1080p30','4k60')) {
    $frames = if ($size -eq '1080p30') { 30 } else { 60 }
    foreach ($format in @('apch','ap4h-alpha16')) {
        foreach ($order in @('tff','bff')) {
            $name = "$format-$order-$size"
            $input = Join-Path $root "build/m4-performance/$name.mov"
            $trial = Join-Path $out $name
            & python (Join-Path $root 'scripts/benchmark-d3d11-plugin.py') $input `
                --modes dx11-direct --repeats 3 --loops 3 --warmup 5 `
                --frames-per-loop $frames --seeks 0 --startups 0 `
                --build-dir (Join-Path $root 'build/vs18') --out $trial *> (Join-Path $out "$name.log")
            if ($LASTEXITCODE) { throw "DX11 direct測定失敗: $name" }
            $row = @(Get-Content (Join-Path $trial 'medians.json') -Raw | ConvertFrom-Json)[0]
            if (!$row -or $row.repeats -ne 3 -or $row.steady_fps -le 0 -or
                $row.gpu_completion_wait) { throw "DX11 direct測定値が不正: $name" }
            $records += [pscustomobject]@{
                input="build/m4-performance/$name.mov"
                sha256=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant()
                frames_per_loop=$frames; repeats=3; loops=3
                median_steady_fps=$row.steady_fps
                realtime_4k60=$(if ($size -eq '4k60') { $row.steady_fps -ge 60 } else { $null })
            }
            $records | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
            Write-Output "$name direct_fps=$($row.steady_fps)"
        }
    }
}
if (@($records | Where-Object { $_.realtime_4k60 -eq $false }).Count) {
    throw '4K60リアルタイムゲート未達。summary.jsonを参照'
}
