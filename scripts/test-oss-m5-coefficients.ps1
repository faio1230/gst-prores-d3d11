# M5端数寸法の全2枚ずつで、製品VLD CSOとIDCT画素を固定SDK CPUに照合する。
[CmdletBinding()]
param([string]$OutDir = 'build/oss-prepublish/m5-coeff')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$validator = Join-Path $root 'build/vs18/Release/prores_dx11_coeff.exe'
$shader = Join-Path $root 'build/vs18/plugins/Release/prores_vld.cso'
$sdk = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$savedPath = $env:PATH
$records = @()
try {
    $env:PATH = "$sdk;$savedPath"
    foreach ($name in @('apch-non16.mov','ap4h-odd.mov')) {
        $inputFile = Join-Path $root "media/feature-matrix-2026-09-24/$name"
        $count = [long]0
        $maximum = 0
        for ($frame = 0; $frame -lt 2; ++$frame) {
            $lines = @(& $validator $inputFile $shader "--frame=$frame" 2>&1 | ForEach-Object { "$_" })
            $lines | Add-Content -LiteralPath (Join-Path $out "$name.jsonl") -Encoding utf8
            if ($LASTEXITCODE) { throw "M5端数係数検査失敗: $name frame=$frame" }
            $item = $lines | Select-Object -Last 1 | ConvertFrom-Json
            $pixelMax = ($item.pixel_differences | Measure-Object max_abs -Maximum).Maximum
            if (!$item.passed -or $item.mismatches -ne 0 -or $item.shader_errors -ne 0 -or
                $pixelMax -gt 1) { throw "M5端数係数判定不一致: $name frame=$frame" }
            $count += [long]$item.coefficients
            $maximum = [math]::Max($maximum, $pixelMax)
        }
        $records += [pscustomobject]@{input="media/feature-matrix-2026-09-24/$name"
            sha256=(Get-FileHash $inputFile -Algorithm SHA256).Hash.ToLowerInvariant()
            frames=2;coefficients_checked=$count;coefficient_mismatches=0
            shader_errors=0;pixel_max_abs=$maximum;passed=$true}
        $records | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
        Write-Host "$name`: 係数${count}個一致、画素最大差$maximum"
    }
} finally { $env:PATH = $savedPath }
