# 診断ログ付きの旧版Map待ちと新版ring回収待ちを同じ4K60素材で交互比較する。
[CmdletBinding()]
param(
    [ValidateRange(1, 4)][int]$Pairs = 2,
    [ValidateRange(1, 20)][int]$Loops = 10,
    [switch]$Resume,
    [string]$OutDir = 'results/vld-async-wait-ab-2026-09-24',
    [string]$OldStage = 'build/vs18/stage-vld-async-old-a3b4315',
    [string]$NewStage = 'build/vs18/stage-vld-async-new-b981a32',
    [string]$OldHead = 'a3b43158534d56449877049e42cce355f40de326',
    [string]$NewHead = 'b981a32dded81a3ac315a391a3459c8bbe281296'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$source = Join-Path $root 'media/reference-dji-nature-4k60-rec709-hq.mov'
$python = (Get-Command python -ErrorAction Stop).Source
$stages = @{ old = Join-Path $root $OldStage; new = Join-Path $root $NewStage }
$heads = @{ old = $OldHead; new = $NewHead }
if ((Test-Path -LiteralPath $out) -and !$Resume) {
    throw "既存結果を上書きしません: $out"
}
if (!(Test-Path -LiteralPath $source)) { throw "素材がありません: $source" }
$hashes = @{}
$shaderHashes = @{}
foreach ($variant in @('old', 'new')) {
    $manifest = Get-Content -LiteralPath (Join-Path $stages[$variant] 'manifest.json') -Raw |
        ConvertFrom-Json
    if ($manifest.repository_head_at_staging -ne $heads[$variant] -or
        $manifest.working_tree_dirty_at_staging -or !$manifest.source_rebuild.passed) {
        throw "独立ステージの由来を確認できません: $variant"
    }
    foreach ($name in @('gstproresd3d11.dll', 'prores_vld.cso',
                         'prores_idct_unorm.cso', 'prores_rgb.cso')) {
        $expected = $manifest.files | Where-Object name -EQ $name | Select-Object -First 1
        $file = Join-Path $stages[$variant] $name
        if (!$expected -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne
            $expected.sha256) { throw "artifactのSHA256が一致しません: $variant/$name" }
        if ($name -eq 'gstproresd3d11.dll') { $hashes[$variant] = $expected.sha256.ToLowerInvariant() }
        else { $shaderHashes["$variant/$name"] = $expected.sha256.ToLowerInvariant() }
    }
}
$shaderMismatch = @(@('prores_vld.cso', 'prores_idct_unorm.cso', 'prores_rgb.cso') |
    Where-Object { $shaderHashes["old/$_"] -ne $shaderHashes["new/$_"] })
if ($hashes.old -eq $hashes.new -or $shaderMismatch.Count) {
    throw 'DLL差分またはshader同一性を確認できません'
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$savedCpu = $env:PRORES_DX11_CPU_TIMING
$savedGpu = $env:PRORES_DX11_GPU_TIMING
$savedDebug = $env:GST_DEBUG
$records = @()
try {
    $env:PRORES_DX11_CPU_TIMING = '1'
    Remove-Item Env:PRORES_DX11_GPU_TIMING -ErrorAction SilentlyContinue
    $env:GST_DEBUG = 'proresd3d11dec:4'
    for ($pair = 0; $pair -lt $Pairs; ++$pair) {
        $order = if ($pair % 2 -eq 0) { @('old', 'new') } else { @('new', 'old') }
        foreach ($variant in $order) {
            $tag = "p$pair-$variant"
            $trial = Join-Path $out $tag
            $log = Join-Path $out "$tag-benchmark.log"
            if (!(Test-Path -LiteralPath $trial)) {
                & $python (Join-Path $root 'scripts/benchmark-d3d11-display.py') $source `
                    --loops $Loops --repeats 1 --preroll --native-rgb --trace-sink-return `
                    --settle-ms 150 --plugin-dir $stages[$variant] --out $trial *> $log
                if ($LASTEXITCODE) { throw "診断試行に失敗: $tag ($log)" }
            } elseif (!$Resume) {
                throw "既存試行を上書きしません: $trial"
            }
            $trialJson = Get-ChildItem -LiteralPath $trial -Filter '*.json' |
                Select-Object -First 1 -ExpandProperty FullName
            $stderrLog = Get-ChildItem -LiteralPath $trial -Filter '*.stderr.log' |
                Select-Object -First 1 -ExpandProperty FullName
            if (!$trialJson -or !$stderrLog) { throw "試行記録が不足: $tag" }
            $metrics = Get-Content -LiteralPath $trialJson -Raw | ConvertFrom-Json
            $expected = $metrics.source_frames * $Loops
            if ($metrics.plugin_sha256 -ne $hashes[$variant] -or
                $metrics.present_sync_interval -ne 0 -or !$metrics.sink_clock_sync -or
                $metrics.decoder_no_qos -or
                $metrics.rendered + $metrics.stages.end_to_end_missing -ne $expected -or
                $metrics.stages.missing_before_decoder_count -ne 0 -or
                $metrics.stages.missing_after_converter_count -ne 0 -or
                $metrics.stages.missing_after_decoder_count + $metrics.dropped -ne
                    $metrics.stages.end_to_end_missing) {
                throw "通常QoSまたはPTS件数が不一致: $tag"
            }
            $stageSummary = Join-Path $out "$tag-cpu-summary.json"
            & $python (Join-Path $root 'scripts/summarize-decoder-cpu-stages.py') `
                $trialJson $stderrLog --allow-sink-drop --out $stageSummary `
                *> (Join-Path $out "$tag-cpu.log")
            if ($LASTEXITCODE) { throw "CPU段階のPTS照合に失敗: $tag" }
            $cpu = Get-Content -LiteralPath $stageSummary -Raw | ConvertFrom-Json
            if ($cpu.cpu_stage_rows -ne $expected -or
                ($variant -eq 'new' -and !$cpu.field_ms.retire_wait_ms)) {
                throw "CPU段階が不足: $tag"
            }
            $wait = if ($variant -eq 'old') { $cpu.field_ms.vld_map_ms }
                else { $cpu.field_ms.retire_wait_ms }
            $records += [pscustomobject]@{
                tag = $tag; pair = $pair; version = $variant; expected_frames = $expected
                rendered = $metrics.rendered
                decoder_qos_missing = $metrics.stages.missing_after_decoder_count
                sink_dropped = $metrics.dropped
                wait_p99_ms = $wait.p99; wait_max_ms = $wait.max
                decode_p99_ms = $cpu.field_ms.backend_ms.p99
                plugin_sha256 = $hashes[$variant]
            }
            $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
                (Join-Path $out 'trial-summary.json') -Encoding utf8
            Write-Host "$tag`: 待ちp99=$($wait.p99)ms、最大=$($wait.max)ms、decode p99=$($cpu.field_ms.backend_ms.p99)ms"
        }
    }
} finally {
    if ($null -eq $savedCpu) { Remove-Item Env:PRORES_DX11_CPU_TIMING -ErrorAction SilentlyContinue }
    else { $env:PRORES_DX11_CPU_TIMING = $savedCpu }
    if ($null -eq $savedGpu) { Remove-Item Env:PRORES_DX11_GPU_TIMING -ErrorAction SilentlyContinue }
    else { $env:PRORES_DX11_GPU_TIMING = $savedGpu }
    if ($null -eq $savedDebug) { Remove-Item Env:GST_DEBUG -ErrorAction SilentlyContinue }
    else { $env:GST_DEBUG = $savedDebug }
}
