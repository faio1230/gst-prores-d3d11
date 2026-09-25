# 左寄せクロマの独立BT.709式を、M1/M2と公開BT.709実写の全画素で検査する。
[CmdletBinding()]
param([string]$OutDir = 'build/oss-prepublish/rgb-left')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null
$m1 = @((Get-Content (Join-Path $root 'results/profile-422-expansion-2026-09-24/fixtures.json') -Raw | ConvertFrom-Json)) +
      @((Get-Content (Join-Path $root 'results/profile-422-expansion-2026-09-24/fixtures-4k.json') -Raw | ConvertFrom-Json))
$m2 = @((Get-Content (Join-Path $root 'results/prores-m2-2026-09-24/fixtures-rgb.json') -Raw | ConvertFrom-Json))
if ($m1.Count -ne 11 -or $m2.Count -ne 18) { throw 'M1/M2のRGB素材数が不正です' }
$cases = @()
foreach ($item in $m1) { $cases += [pscustomobject]@{ group='M1'; input=$item.input; frames=$item.frames; sha256=$item.sha256 } }
foreach ($item in $m2) { $cases += [pscustomobject]@{ group='M2'; input=$item.input; frames=$item.frames; sha256=$item.sha256 } }
foreach ($item in @(
    @{input='media/reference-proton-rec709-hq.mov'; frames=50},
    @{input='media/reference-dji-nature-4k24-hq.mov'; frames=129},
    @{input='media/reference-dji-nature-4k60-rec709-hq.mov'; frames=480}
)) { $cases += [pscustomobject]@{ group='公開HQ'; input=$item.input; frames=$item.frames; sha256=$null } }
$rows = @()
for ($index = 0; $index -lt $cases.Count; ++$index) {
    $item = $cases[$index]
    $inputFile = Join-Path $root $item.input
    $actualHash = (Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($item.sha256 -and $item.sha256 -ne $actualHash) { throw "素材SHA256不一致: $($item.input)" }
    $tag = '{0:d2}-{1}' -f $index, [IO.Path]::GetFileNameWithoutExtension($inputFile)
    $json = Join-Path $out "$tag.json"
    & python (Join-Path $root 'scripts/compare-d3d11-real.py') $inputFile `
        --mode rgb_element_formula --frames $item.frames --streaming --out $json `
        *> (Join-Path $out "$tag.log")
    if ($LASTEXITCODE) { throw "RGB独立式との全画素比較失敗: $($item.input)" }
    $result = Get-Content $json -Raw | ConvertFrom-Json
    $maximum = ($result.channels | Measure-Object max_abs -Maximum).Maximum
    if (!$result.passed -or $result.frames -ne $item.frames -or $maximum -gt 1) {
        throw "RGB独立式の判定不一致: $($item.input)"
    }
    $rows += [pscustomobject]@{ group=$item.group; input=$item.input; sha256=$actualHash
        frames=$item.frames; rgb_max_abs=$maximum; passed=$true }
    $rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
    Write-Host "$tag`: $($item.frames)枚、RGB最大差$maximum"
}
