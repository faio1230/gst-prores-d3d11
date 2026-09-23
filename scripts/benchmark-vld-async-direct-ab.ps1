# VLD非同期化の旧版・新版を、独立ステージから交互に1080p/4K direct測定する。
[CmdletBinding()]
param(
    [ValidateRange(1, 5)][int]$Pairs = 4,
    [string]$OutDir = 'results/vld-async-direct-ab-2026-09-24',
    [string]$OldStage = 'build/vs18/stage-vld-async-old-a3b4315',
    [string]$NewStage = 'build/vs18/stage-vld-async-new-b981a32',
    [string]$OldHead = 'a3b43158534d56449877049e42cce355f40de326',
    [string]$NewHead = 'b981a32dded81a3ac315a391a3459c8bbe281296',
    [ValidatePattern('^[a-z0-9-]+$')][string]$BuildLabel = 'vld-async-direct'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$build = Join-Path $root 'build/vs18'
$bench = Join-Path $build 'Release/d3d11_plugin_bench.exe'
$python = (Get-Command python -ErrorAction Stop).Source
$sources = @((Join-Path $root 'media/synthetic-1080p60-hq.mov'),
             (Join-Path $root 'media/synthetic-2160p60-hq.mov'))
$stages = @{ old = Join-Path $root $OldStage; new = Join-Path $root $NewStage }
$heads = @{ old = $OldHead; new = $NewHead }
$names = @('gstproresd3d11.dll', 'prores_vld.cso', 'prores_idct_unorm.cso', 'prores_rgb.cso')
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
foreach ($file in @($bench) + $sources) {
    if (!(Test-Path -LiteralPath $file)) { throw "必要なファイルがありません: $file" }
}
$hashes = @{}
$shaderHashes = @{}
$variantBuilds = @{}
foreach ($variant in @('old', 'new')) {
    $stage = $stages[$variant]
    $manifest = Get-Content -LiteralPath (Join-Path $stage 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.repository_head_at_staging -ne $heads[$variant] -or
        $manifest.working_tree_dirty_at_staging -or !$manifest.source_rebuild.passed) {
        throw "独立ステージの由来を確認できません: $variant"
    }
    $variantBuild = Join-Path $build "${BuildLabel}-$variant"
    if (Test-Path -LiteralPath $variantBuild) { throw "既存配置を上書きしません: $variantBuild" }
    $pluginDir = Join-Path $variantBuild 'plugins/Release'
    $exeDir = Join-Path $variantBuild 'Release'
    New-Item -ItemType Directory -Force -Path $pluginDir, $exeDir | Out-Null
    Copy-Item -LiteralPath $bench -Destination $exeDir
    foreach ($name in $names) {
        $source = Join-Path $stage $name
        $expected = $manifest.files | Where-Object name -EQ $name | Select-Object -First 1
        if (!$expected -or !(Test-Path -LiteralPath $source) -or
            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $expected.sha256) {
            throw "ステージのSHA256が一致しません: $variant/$name"
        }
        Copy-Item -LiteralPath $source -Destination $pluginDir
        if ($name -eq 'gstproresd3d11.dll') { $hashes[$variant] = $expected.sha256.ToLowerInvariant() }
        else { $shaderHashes["$variant/$name"] = $expected.sha256.ToLowerInvariant() }
    }
    $variantBuilds[$variant] = $variantBuild
}
$shaderMismatch = @(@('prores_vld.cso', 'prores_idct_unorm.cso', 'prores_rgb.cso') |
    Where-Object { $shaderHashes["old/$_"] -ne $shaderHashes["new/$_"] })
if ($hashes.old -eq $hashes.new -or $shaderMismatch.Count) {
    throw 'DLL差分またはshader同一性を確認できません'
}
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
for ($pair = 0; $pair -lt $Pairs; ++$pair) {
    $order = if ($pair % 2 -eq 0) { @('old', 'new') } else { @('new', 'old') }
    foreach ($variant in $order) {
        $tag = "p$pair-$variant"
        $trial = Join-Path $out $tag
        $log = Join-Path $out "$tag.log"
        & $python (Join-Path $root 'scripts/benchmark-d3d11-plugin.py') @sources `
            --modes dx11-direct --repeats 1 --loops 3 --warmup 30 `
            --frames-per-loop 180 --seeks 0 --startups 0 `
            --build-dir $variantBuilds[$variant] --out $trial *> $log
        if ($LASTEXITCODE) { throw "direct測定に失敗: $tag ($log)" }
        $medians = Get-Content -LiteralPath (Join-Path $trial 'medians.json') -Raw | ConvertFrom-Json
        if (@($medians).Count -ne 2) { throw "1080p/4Kの結果が不足: $tag" }
        foreach ($item in $medians) {
            if ($item.mode -ne 'dx11-direct' -or $item.steady_fps -le 0 -or
                $item.gpu_completion_wait) { throw "direct条件が不正: $tag" }
            $records += [pscustomobject]@{
                tag = $tag; pair = $pair; version = $variant; input = $item.input
                steady_fps = $item.steady_fps; plugin_sha256 = $hashes[$variant]
            }
        }
        $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
            (Join-Path $out 'trial-summary.json') -Encoding utf8
        Write-Host "$tag`: 1080p/4K direct完了"
    }
}
$summary = @()
foreach ($inputName in @('synthetic-1080p60-hq.mov', 'synthetic-2160p60-hq.mov')) {
    $values = @{}
    foreach ($variant in @('old', 'new')) {
        $sorted = @($records | Where-Object { $_.input -eq $inputName -and $_.version -eq $variant } |
            ForEach-Object { [double]$_.steady_fps } | Sort-Object)
        if ($sorted.Count -ne $Pairs) { throw "反復数が不足: $inputName/$variant" }
        $middle = [int][math]::Floor($Pairs / 2)
        $values[$variant] = if ($Pairs % 2) { $sorted[$middle] }
            else { ($sorted[$middle - 1] + $sorted[$middle]) / 2 }
    }
    $summary += [pscustomobject]@{
        input = $inputName; old_median_fps = $values.old; new_median_fps = $values.new
        change_percent = 100 * ($values.new / $values.old - 1)
    }
}
[pscustomobject]@{ pairs = $Pairs; old_head = $heads.old; new_head = $heads.new;
    results = $summary } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath `
    (Join-Path $out 'summary.json') -Encoding utf8
$summary | Format-Table | Out-String | Write-Host
