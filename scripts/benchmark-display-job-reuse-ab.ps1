# cleanな旧版・ジョブ再利用版の内部ステージを、標準sink/通常QoSで交互比較する。
[CmdletBinding()]
param(
    [ValidateRange(1, 5)][int]$Pairs = 2,
    [ValidateRange(1, 20)][int]$Loops = 10,
    [string]$OutDir = 'results/display-job-reuse-ab-2026-09-24',
    [string]$Source = 'media/reference-dji-nature-4k60-rec709-hq.mov',
    [string]$OldStage = 'build/vs18/stage-ac-boundary-fdff052',
    [string]$NewStage = 'build/vs18/stage-cpu-job-reuse-9e4b3b9'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$sourcePath = Join-Path $root $Source
$python = (Get-Command python -ErrorAction Stop).Source
if (Test-Path -LiteralPath $out) { throw "既存の結果を上書きしません: $out" }
if (!(Test-Path -LiteralPath $sourcePath)) { throw "素材がありません: $sourcePath" }
$stages = @{
    old = Join-Path $root $OldStage
    new = Join-Path $root $NewStage
}
$hashes = @{}
$shaderHashes = @{}
foreach ($label in @('old', 'new')) {
    $stage = $stages[$label]
    $manifestPath = Join-Path $stage 'manifest.json'
    if (!(Test-Path -LiteralPath $manifestPath)) { throw "ステージがありません: $stage" }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.working_tree_dirty_at_staging -or !$manifest.source_rebuild.passed) {
        throw "cleanなソース再ビルド済みステージではありません: $stage"
    }
    foreach ($name in @('gstproresd3d11.dll', 'prores_vld.cso',
                         'prores_idct_unorm.cso', 'prores_rgb.cso')) {
        $expected = $manifest.files | Where-Object name -eq $name | Select-Object -First 1
        $file = Join-Path $stage $name
        if (!$expected -or !(Test-Path -LiteralPath $file) -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected.sha256) {
            throw "ステージのSHA256が一致しません: $file"
        }
        if ($name -eq 'gstproresd3d11.dll') { $hashes[$label] = $expected.sha256.ToLowerInvariant() }
        else { $shaderHashes["$label/$name"] = $expected.sha256.ToLowerInvariant() }
    }
}
if ($hashes.old -eq $hashes.new) { throw '旧版と新版のDLLが同一です' }
foreach ($name in @('prores_vld.cso', 'prores_idct_unorm.cso', 'prores_rgb.cso')) {
    if ($shaderHashes["old/$name"] -ne $shaderHashes["new/$name"]) {
        throw "旧版と新版のshaderが異なります: $name"
    }
}
New-Item -ItemType Directory -Path $out | Out-Null
$savedTiming = $env:PRORES_DX11_CPU_TIMING
$savedDebug = $env:GST_DEBUG
$records = @()
try {
    Remove-Item Env:PRORES_DX11_CPU_TIMING -ErrorAction SilentlyContinue
    Remove-Item Env:GST_DEBUG -ErrorAction SilentlyContinue
    for ($pair = 0; $pair -lt $Pairs; $pair++) {
        $order = if ($pair % 2 -eq 0) { @('old', 'new') } else { @('new', 'old') }
        foreach ($label in $order) {
            $tag = "p$pair-$label"
            $trial = Join-Path $out $tag
            & $python scripts/benchmark-d3d11-display.py $sourcePath `
                --loops $Loops --repeats 1 --preroll --native-rgb `
                --trace-sink-return --trace-window-state --settle-ms 150 `
                --plugin-dir $stages[$label] --out $trial `
                *> (Join-Path $out "$tag-benchmark.log")
            if ($LASTEXITCODE) { throw "表示試行に失敗: $tag" }
            $trialJson = Get-ChildItem -LiteralPath $trial -Filter '*.json' |
                Select-Object -First 1 -ExpandProperty FullName
            if (!$trialJson) { throw "試行JSONがありません: $tag" }
            $metrics = Get-Content -LiteralPath $trialJson -Raw | ConvertFrom-Json
            $expected = $metrics.source_frames * $Loops
            if ($metrics.plugin_sha256 -ne $hashes[$label] -or
                $metrics.present_sync_interval -ne 0 -or !$metrics.sink_clock_sync -or
                $metrics.decoder_no_qos -or $metrics.lossless_sink_policy -or
                $metrics.postrgb_queue -or $metrics.dropped -ne 0 -or
                $metrics.rendered + $metrics.stages.end_to_end_missing -ne $expected -or
                $metrics.stages.missing_before_decoder_count -ne 0 -or
                $metrics.stages.missing_after_converter_count -ne 0) {
                throw "試行条件またはPTS枚数が不一致: $tag"
            }
            $records += [pscustomobject]@{
                tag = $tag
                version = $label
                plugin_sha256 = $hashes[$label]
                expected_frames = $expected
                rendered = $metrics.rendered
                decoder_qos_missing = $metrics.stages.end_to_end_missing
                qos_messages = $metrics.qos_messages
                p99_present_interval_ms = $metrics.interval_p99_ms
                max_sink_push_ms = $metrics.stages.stage_latency.sink_push_to_sink_return_ms.max
            }
            $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
                (Join-Path $out 'trial-summary.json') -Encoding utf8
            Write-Host "$tag`: $($metrics.rendered)/$expected、decoder QoS欠落=$($metrics.stages.end_to_end_missing)"
        }
    }
} finally {
    if ($null -eq $savedTiming) { Remove-Item Env:PRORES_DX11_CPU_TIMING -ErrorAction SilentlyContinue }
    else { $env:PRORES_DX11_CPU_TIMING = $savedTiming }
    if ($null -eq $savedDebug) { Remove-Item Env:GST_DEBUG -ErrorAction SilentlyContinue }
    else { $env:GST_DEBUG = $savedDebug }
}
