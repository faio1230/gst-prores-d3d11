# AC境界修正前後のDLL/CSOを同じホストで交互測定する。正常な合成素材だけを入力する。
[CmdletBinding()]
param(
    [ValidateRange(1, 4)][int]$Pairs = 2,
    [string]$OutDir = 'results/ac-boundary-ab-2026-09-24'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$build = Join-Path $root 'build/vs18'
$bench = Join-Path $build 'Release/d3d11_plugin_bench.exe'
$python = (Get-Command python -ErrorAction Stop).Source
$source1080 = Join-Path $root 'media/synthetic-1080p60-hq.mov'
$source4k = Join-Path $root 'media/synthetic-2160p60-hq.mov'
$versions = [ordered]@{
    old = @{ stage = 'build/vs18/stage-d3d11-map-timeout-2a64823';
             head = '2a64823794fc51ecd57e30ee86b6fe9991facda5' }
    new = @{ stage = 'build/vs18/stage-ac-boundary-fdff052';
             head = 'fdff05220eb7009c14e534e5697d26413c8f2f91' }
}
if (Test-Path -LiteralPath $out) { throw "既存の比較結果は上書きしません: $out" }
foreach ($required in @($bench, $source1080, $source4k)) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
$artifacts = @('gstproresd3d11.dll', 'prores_vld.cso',
               'prores_idct_unorm.cso', 'prores_rgb.cso')
foreach ($variant in $versions.Keys) {
    $stage = Join-Path $root $versions[$variant].stage
    $manifestName = if ($variant -eq 'old') {
        'results/stage-d3d11-map-timeout-2026-09-24.json'
    } else { 'results/stage-ac-boundary-2026-09-24.json' }
    $manifest = Get-Content -LiteralPath (Join-Path $root $manifestName) -Raw |
        ConvertFrom-Json
    if ($manifest.repository_head_at_staging -ne $versions[$variant].head -or
        $manifest.working_tree_dirty_at_staging -or !$manifest.source_rebuild.passed) {
        throw "ステージの由来を確認できません: $variant"
    }
    $variantBuild = Join-Path $build "ac-boundary-ab-$variant"
    $pluginDir = Join-Path $variantBuild 'plugins/Release'
    $exeDir = Join-Path $variantBuild 'Release'
    $fresh = !(Test-Path -LiteralPath $variantBuild)
    if ($fresh) {
        New-Item -ItemType Directory -Path $pluginDir, $exeDir -Force | Out-Null
        Copy-Item -LiteralPath $bench -Destination $exeDir
    }
    $copiedBench = Join-Path $exeDir 'd3d11_plugin_bench.exe'
    if (!(Test-Path -LiteralPath $copiedBench) -or
        (Get-FileHash -LiteralPath $copiedBench -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $bench -Algorithm SHA256).Hash) {
        throw "比較用ベンチ実行ファイルが現行版と違います: $variant"
    }
    foreach ($name in $artifacts) {
        if ($fresh) { Copy-Item -LiteralPath (Join-Path $stage $name) -Destination $pluginDir }
        $expected = $manifest.files | Where-Object name -EQ $name | Select-Object -First 1
        $copy = Join-Path $pluginDir $name
        if (!(Test-Path -LiteralPath $copy)) { throw "比較用artifactがありません: $variant/$name" }
        $actual = (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash
        if (!$expected -or $actual -ne $expected.sha256) {
            throw "配置したartifactのSHA256が違います: $variant/$name"
        }
    }
}
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
for ($pair = 0; $pair -lt $Pairs; $pair++) {
    $order = if ($pair % 2 -eq 0) { @('old', 'new') } else { @('new', 'old') }
    foreach ($variant in $order) {
        $trialOut = Join-Path $out "pair$pair-$variant"
        $variantBuild = Join-Path $build "ac-boundary-ab-$variant"
        $log = Join-Path $out "pair$pair-$variant.log"
        & $python (Join-Path $root 'scripts/benchmark-d3d11-plugin.py') `
            $source1080 $source4k --modes dx11-direct dx11-download --repeats 1 `
            --loops 3 --warmup 30 --frames-per-loop 180 --seeks 0 --startups 0 `
            --build-dir $variantBuild --out $trialOut *> $log
        if ($LASTEXITCODE) { throw "交互性能測定に失敗: pair$pair-$variant ($log)" }
        $medians = Get-Content -LiteralPath (Join-Path $trialOut 'medians.json') -Raw |
            ConvertFrom-Json
        foreach ($item in $medians) {
            $records += [pscustomobject]@{
                pair = $pair
                variant = $variant
                input = $item.input
                mode = $item.mode
                steady_fps = $item.steady_fps
                interval_p99_ms = $item.interval_p99_ms
            }
        }
        $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
            (Join-Path $out 'trial-summary.json') -Encoding utf8
        Write-Host "pair$pair-$variant`: $($medians.Count)条件完了"
    }
}
