# M2全18素材のRGB10A2 D3D11Memory経路を独立BT.709式と全画素比較する。
[CmdletBinding()]
param([string]$OutDir = 'results/prores-m2-2026-09-24/rgb')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fixtureFile = Join-Path $root 'results/prores-m2-2026-09-24/fixtures-rgb.json'
$fixtures = @(Get-Content -LiteralPath $fixtureFile -Raw | ConvertFrom-Json)
$out = Join-Path $root $OutDir
if ($fixtures.Count -ne 18) { throw "M2 RGB素材数が不正: $($fixtures.Count)" }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$records = @()
foreach ($item in $fixtures) {
    $input = Join-Path $root $item.input
    if (!(Test-Path -LiteralPath $input) -or
        (Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
        throw "RGB素材SHA256が不一致: $($item.input)"
    }
    $name = [IO.Path]::GetFileNameWithoutExtension($input)
    $resultPath = Join-Path $out "$name.json"
    & python (Join-Path $root 'scripts/compare-d3d11-real.py') $input `
        --mode rgb_element_formula --frames $item.frames --streaming --out $resultPath `
        *> (Join-Path $out "$name-run.log")
    if ($LASTEXITCODE) { throw "RGB全画素比較失敗: $name" }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    $maximum = ($result.channels | Measure-Object max_abs -Maximum).Maximum
    if (!$result.passed -or $result.frames -ne $item.frames -or $maximum -gt 1) {
        throw "RGB画素差・枚数が不正: $name"
    }
    $records += [pscustomobject]@{
        input=$item.input; sha256=$item.sha256; source_sha256=$item.source_sha256
        fourcc=$item.fourcc; chroma=$item.chroma; bit_depth=$item.bit_depth
        width=$item.width; height=$item.height; frames=$item.frames
        rgb_formula_max_abs=[int]$maximum; rgb_d3d11memory=$true; passed=$true
    }
    $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
    Write-Host "$name`: $($item.frames)枚、RGB式との差最大$maximum"
}
