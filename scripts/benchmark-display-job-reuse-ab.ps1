# cleanな旧版・ジョブ再利用版の内部ステージを、標準sink/通常QoSで交互比較する。
[CmdletBinding()]
param(
    [ValidateRange(1, 5)][int]$Pairs = 2,
    [ValidateRange(1, 20)][int]$Loops = 10,
    [switch]$CaptureOS,
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
$presentMon = Join-Path $root 'build/vs18/diagnostic-tools/PresentMon-2.6.0-x64.exe'
if (Test-Path -LiteralPath $out) { throw "既存の結果を上書きしません: $out" }
if (!(Test-Path -LiteralPath $sourcePath)) { throw "素材がありません: $sourcePath" }
if ($CaptureOS -and !(Test-Path -LiteralPath $presentMon)) {
    throw "PresentMonがありません: $presentMon"
}
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
            $monitor = $null
            $session = 'IC' + (Get-Date -Format 'MMddHHmmss') + $pair + $label.Substring(0, 1)
            $capture = Join-Path $out "$tag-presentmon.csv"
            $trialCompleted = $false
            try {
                if ($CaptureOS) {
                    $duration = 10 + 8 * $Loops
                    $monitor = Start-Process -FilePath $presentMon -ArgumentList @(
                        '--process_name', 'd3d11_display_bench.exe', '--output_file', $capture,
                        '--qpc_time_ms', '--write_display_metadata', '--set_circular_buffer_size', '32768',
                        '--timed', [string]$duration, '--terminate_after_timed', '--no_console_stats',
                        '--session_name', $session
                    ) -WindowStyle Hidden -PassThru `
                        -RedirectStandardOutput (Join-Path $out "$tag-presentmon.log") `
                        -RedirectStandardError (Join-Path $out "$tag-presentmon-error.log")
                    Start-Sleep -Milliseconds 1200
                }
                & $python scripts/benchmark-d3d11-display.py $sourcePath `
                    --loops $Loops --repeats 1 --preroll --native-rgb `
                    --trace-sink-return --trace-window-state --settle-ms 150 `
                    --plugin-dir $stages[$label] --out $trial `
                    *> (Join-Path $out "$tag-benchmark.log")
                if ($LASTEXITCODE) { throw "表示試行に失敗: $tag" }
                if ($CaptureOS -and (!$monitor.WaitForExit(30000) -or $monitor.ExitCode -ne 0 -or
                    !(Test-Path -LiteralPath $capture))) { throw "PresentMon取得失敗: $tag" }
            $trialJson = Get-ChildItem -LiteralPath $trial -Filter '*.json' |
                Select-Object -First 1 -ExpandProperty FullName
            if (!$trialJson) { throw "試行JSONがありません: $tag" }
            $metrics = Get-Content -LiteralPath $trialJson -Raw | ConvertFrom-Json
            $expected = $metrics.source_frames * $Loops
            if ($metrics.plugin_sha256 -ne $hashes[$label] -or
                $metrics.present_sync_interval -ne 0 -or !$metrics.sink_clock_sync -or
                $metrics.decoder_no_qos -or $metrics.lossless_sink_policy -or
                $metrics.postrgb_queue -or
                $metrics.rendered + $metrics.stages.end_to_end_missing -ne $expected -or
                $metrics.stages.missing_before_decoder_count -ne 0 -or
                $metrics.stages.missing_after_converter_count -ne 0 -or
                $metrics.stages.missing_after_decoder_count + $metrics.dropped -ne
                    $metrics.stages.end_to_end_missing) {
                throw "試行条件またはPTS枚数が不一致: $tag"
            }
            $present = $null
            if ($CaptureOS) {
                $presentSummary = Join-Path $out "$tag-presentmon-summary.json"
                & $python scripts/summarize-presentmon-display.py $capture $trial `
                    --out $presentSummary *> (Join-Path $out "$tag-presentmon-summary.log")
                if ($LASTEXITCODE) { throw "PresentMon PTS照合失敗: $tag" }
                $present = Get-Content -LiteralPath $presentSummary -Raw | ConvertFrom-Json
                if ($present.coverage_sufficient_trials -ne 1 -or
                    $present.pts_aligned_trials -ne 1 -or
                    $present.interior_uncaptured_aligned -ne 0) {
                    throw "PresentMon内側捕捉が不完全: $tag"
                }
            }
            $records += [pscustomobject]@{
                tag = $tag
                version = $label
                plugin_sha256 = $hashes[$label]
                expected_frames = $expected
                rendered = $metrics.rendered
                decoder_qos_missing = $metrics.stages.missing_after_decoder_count
                sink_dropped = $metrics.dropped
                qos_messages = $metrics.qos_messages
                p99_present_interval_ms = $metrics.interval_p99_ms
                max_sink_push_ms = $metrics.stages.stage_latency.sink_push_to_sink_return_ms.max
                os_not_displayed_interior = if ($present) { $present.interior_captured_but_not_displayed_aligned } else { $null }
                os_interior_frames = if ($present) { $present.interior_source_frames_aligned } else { $null }
                max_os_display_gap_ms = if ($present) { $present.max_display_gap_ms } else { $null }
            }
            $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
                (Join-Path $out 'trial-summary.json') -Encoding utf8
            Write-Host "$tag`: $($metrics.rendered)/$expected、decoder QoS欠落=$($metrics.stages.missing_after_decoder_count)、sink drop=$($metrics.dropped)"
                $trialCompleted = $true
            } finally {
                if ($CaptureOS -and !$trialCompleted -and $monitor) {
                    if (!$monitor.HasExited) { Stop-Process -Id $monitor.Id -ErrorAction SilentlyContinue }
                    & logman stop $session -ets *> $null
                }
            }
        }
    }
} finally {
    if ($null -eq $savedTiming) { Remove-Item Env:PRORES_DX11_CPU_TIMING -ErrorAction SilentlyContinue }
    else { $env:PRORES_DX11_CPU_TIMING = $savedTiming }
    if ($null -eq $savedDebug) { Remove-Item Env:GST_DEBUG -ErrorAction SilentlyContinue }
    else { $env:GST_DEBUG = $savedDebug }
}
