# M3の24条件48枚のYUV係数を、製品VLD CSOと固定SDK CPUで再照合する。
[CmdletBinding()]
param([string]$OutDir = 'build/oss-prepublish/m3-coeff')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$fixtures = (Get-Content (Join-Path $root 'results/alpha-standard-m3-24-2026-09-24/summary.json') -Raw |
    ConvertFrom-Json).fixtures
$validator = Join-Path $root 'build/vs18/Release/prores_dx11_coeff.exe'
$shader = Join-Path $root 'build/vs18/plugins/Release/prores_vld.cso'
$sdk = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
if ($fixtures.Count -ne 24 -or !(Test-Path $validator) -or !(Test-Path $shader)) {
    throw 'M3の24素材または係数検査器が不足しています'
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$savedPath = $env:PATH
$records = @()
try {
    $env:PATH = "$sdk;$savedPath"
    for ($index = 0; $index -lt $fixtures.Count; ++$index) {
        $fixture = $fixtures[$index]
        $inputFile = Join-Path $root $fixture.file
        if ((Get-FileHash $inputFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $fixture.sha256) {
            throw "素材SHA256不一致: $($fixture.file)"
        }
        $count = [long]0
        $maximum = 0
        for ($frame = 0; $frame -lt $fixture.frames; ++$frame) {
            $lines = @(& $validator $inputFile $shader "--frame=$frame" 2>&1 | ForEach-Object { "$_" })
            $lines | Add-Content -LiteralPath (Join-Path $out "$index.jsonl") -Encoding utf8
            if ($LASTEXITCODE) { throw "係数検査失敗: $($fixture.file) frame=$frame" }
            $item = $lines | Select-Object -Last 1 | ConvertFrom-Json
            $pixelMax = ($item.pixel_differences | Measure-Object max_abs -Maximum).Maximum
            if (!$item.passed -or $item.mismatches -ne 0 -or $item.shader_errors -ne 0 -or
                $pixelMax -gt 1) { throw "係数判定不一致: $($fixture.file) frame=$frame" }
            $count += [long]$item.coefficients
            $maximum = [math]::Max($maximum, $pixelMax)
        }
        $records += [pscustomobject]@{input=$fixture.file;frames=$fixture.frames
            coefficients_checked=$count;coefficient_mismatches=0;shader_errors=0
            pixel_max_abs=$maximum;passed=$true}
        $records | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
        Write-Host "$index`: $($fixture.file) 係数${count}個一致"
    }
} finally { $env:PATH = $savedPath }
