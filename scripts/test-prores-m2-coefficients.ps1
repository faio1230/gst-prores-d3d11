# M2全18素材・全780フレームのGPU VLD係数とIDCT画素をCPU参照と比較する。
[CmdletBinding()]
param([string]$OutDir = 'results/prores-m2-2026-09-24/coefficients')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fixtureDir = Join-Path $root 'results/prores-m2-2026-09-24'
$fixtures = @(Get-Content -LiteralPath (Join-Path $fixtureDir 'fixtures.json') -Raw | ConvertFrom-Json) +
    @(Get-Content -LiteralPath (Join-Path $fixtureDir 'fixtures-4k.json') -Raw | ConvertFrom-Json)
$validator = Join-Path $root 'build/vs18/Release/prores_dx11_coeff.exe'
$shader = Join-Path $root 'build/vs18/plugins/Release/prores_vld.cso'
$ffmpegBin = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin'
$out = Join-Path $root $OutDir
if ($fixtures.Count -ne 18 -or !(Test-Path -LiteralPath $validator) -or
    !(Test-Path -LiteralPath $shader)) { throw 'M2の18素材・検査器・製品CSOが不足' }
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$savedPath = $env:PATH
$records = @()
try {
    $env:PATH = "$ffmpegBin;$savedPath"
    foreach ($item in $fixtures) {
        $input = Join-Path $root $item.input
        if ((Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.sha256) {
            throw "素材SHA256が不一致: $($item.input)"
        }
        $tag = [IO.Path]::GetFileNameWithoutExtension($input)
        $log = Join-Path $out "$tag.jsonl"
        $coefficientCount = [long]0
        $maxPixel = 0
        for ($i = 0; $i -lt $item.frames; ++$i) {
            $lines = @(& $validator $input $shader "--frame=$i" 2>&1 | ForEach-Object { "$_" })
            $code = $LASTEXITCODE
            $lines | Add-Content -LiteralPath $log -Encoding utf8
            if ($code) { throw "GPU係数または画素が不合格: $tag frame=$i ($log)" }
            $row = $lines | Select-Object -Last 1 | ConvertFrom-Json
            $pixelMax = ($row.pixel_differences | Measure-Object max_abs -Maximum).Maximum
            if (!$row.passed -or $row.frame_index -ne $i -or $row.mismatches -ne 0 -or
                $row.shader_errors -ne 0 -or $pixelMax -gt 1) {
                throw "GPU係数JSONが不合格: $tag frame=$i ($log)"
            }
            $coefficientCount += [long]$row.coefficients
            $maxPixel = [math]::Max($maxPixel, $pixelMax)
        }
        $records += [pscustomobject]@{
            input=$item.input; sha256=$item.sha256; frames=$item.frames
            vld_cso_sha256=(Get-FileHash -LiteralPath $shader -Algorithm SHA256).Hash.ToLowerInvariant()
            coefficients_checked=$coefficientCount; coefficient_mismatches=0
            shader_errors=0; pixel_max_abs=$maxPixel; passed=$true
        }
        $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
        Write-Host "$tag`: $($item.frames)枚、係数${coefficientCount}個一致、画素最大差$maxPixel"
    }
} finally {
    $env:PATH = $savedPath
}
