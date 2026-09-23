# 標準Present(0)または診断専用Present(1)でCPU段階とOS表示を同じPTSへ結合する。
[CmdletBinding()]
param(
    [ValidateRange(1, 20)][int]$Loops = 10,
    [ValidateSet(0, 1)][int]$PresentSyncInterval = 0,
    [string]$OutDir = 'results/display-cpu-presentmon-2026-09-24',
    [string]$Source = 'media/reference-dji-nature-4k60-rec709-hq.mov'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/vs18'
$out = Join-Path $root $OutDir
$trial = Join-Path $out 'trial'
$capture = Join-Path $out 'presentmon.csv'
$sourcePath = Join-Path $root $Source
$presentMon = Join-Path $build 'diagnostic-tools/PresentMon-2.6.0-x64.exe'
$bench = Join-Path $build 'Release/d3d11_display_bench.exe'
$python = (Get-Command python -ErrorAction Stop).Source
if (Test-Path -LiteralPath $out) { throw "既存の結果を上書きしません: $out" }
foreach ($required in @($sourcePath, $presentMon, $bench)) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
New-Item -ItemType Directory -Path $trial -Force | Out-Null
$session = 'CP' + (Get-Date -Format 'MMddHHmmss')
$duration = 10 + 8 * $Loops
$monitor = Start-Process -FilePath $presentMon -ArgumentList @(
    '--process_name', 'd3d11_display_bench.exe', '--output_file', $capture,
    '--qpc_time_ms', '--write_display_metadata', '--set_circular_buffer_size', '32768',
    '--timed', [string]$duration, '--terminate_after_timed', '--no_console_stats',
    '--session_name', $session
) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'presentmon.log') `
    -RedirectStandardError (Join-Path $out 'presentmon-error.log')
$savedTiming = $env:PRORES_DX11_CPU_TIMING
$savedDebug = $env:GST_DEBUG
$completed = $false
try {
    Start-Sleep -Milliseconds 1200
    $env:PRORES_DX11_CPU_TIMING = '1'
    $env:GST_DEBUG = 'proresd3d11dec:4'
    $arguments = @('scripts/benchmark-d3d11-display.py', $sourcePath,
        '--loops', [string]$Loops, '--repeats', '1', '--preroll', '--native-rgb',
        '--trace-sink-return', '--trace-window-state', '--settle-ms', '150',
        '--out', $trial)
    if ($PresentSyncInterval -eq 1) { $arguments += @('--present-sync-interval', '1') }
    & $python @arguments *> (Join-Path $out 'benchmark.log')
    if ($LASTEXITCODE) { throw '表示試行に失敗しました' }
    if (!$monitor.WaitForExit(30000) -or $monitor.ExitCode -ne 0 -or
        !(Test-Path -LiteralPath $capture)) { throw 'PresentMon取得に失敗しました' }
    $trialJson = Get-ChildItem -LiteralPath $trial -Filter '*.json' |
        Select-Object -First 1 -ExpandProperty FullName
    $stderrLog = Get-ChildItem -LiteralPath $trial -Filter '*.stderr.log' |
        Select-Object -First 1 -ExpandProperty FullName
    if (!$trialJson -or !$stderrLog) { throw 'ベンチ記録が不足しています' }
    $presentSummary = Join-Path $out 'presentmon-summary.json'
    $cpuSummary = Join-Path $out 'cpu-display-summary.json'
    & $python scripts/summarize-presentmon-display.py $capture $trial --out $presentSummary `
        *> (Join-Path $out 'presentmon-summary.log')
    if ($LASTEXITCODE) { throw 'PresentMon PTS照合に失敗しました' }
    & $python scripts/summarize-decoder-cpu-stages.py $trialJson $stderrLog `
        --presentmon-summary $presentSummary --out $cpuSummary `
        *> (Join-Path $out 'cpu-display-summary.log')
    if ($LASTEXITCODE) { throw 'CPU段階とOS表示のPTS照合に失敗しました' }
    $metrics = Get-Content -LiteralPath $trialJson -Raw | ConvertFrom-Json
    $present = Get-Content -LiteralPath $presentSummary -Raw | ConvertFrom-Json
    $cpu = Get-Content -LiteralPath $cpuSummary -Raw | ConvertFrom-Json
    $expected = $metrics.source_frames * $Loops
    if ($metrics.present_sync_interval -ne $PresentSyncInterval -or !$metrics.sink_clock_sync -or
        $metrics.decoder_no_qos -or $metrics.lossless_sink_policy -or
        $metrics.postrgb_queue -or $metrics.dropped -ne 0 -or
        $metrics.stages.compressed -ne $expected -or
        $metrics.rendered + $metrics.stages.end_to_end_missing -ne $expected -or
        $present.coverage_sufficient_trials -ne 1 -or
        $present.pts_aligned_trials -ne 1 -or
        $present.interior_uncaptured_aligned -ne 0 -or
        $cpu.cpu_stage_rows -ne $expected -or
        ($PresentSyncInterval -eq 1 -and
            ($metrics.present_sync_hook_calls -le 0 -or
             $metrics.present_sync_hook_calls -ne $metrics.present_sync_hook_success))) {
        throw '表示・QoS・PTS照合の試行条件が不完全です'
    }
    $record = [ordered]@{
        source = $Source
        loops = $Loops
        expected_frames = $expected
        rendered = $metrics.rendered
        decoder_qos_missing = $metrics.stages.end_to_end_missing
        sink_dropped = $metrics.dropped
        os_not_displayed_interior = $present.interior_captured_but_not_displayed_aligned
        os_interior_frames = $present.interior_source_frames_aligned
        max_display_gap_ms = $present.max_display_gap_ms
        idct_jobs_p99_ms = $cpu.field_ms.idct_jobs_ms.p99
        vld_map_p99_ms = $cpu.field_ms.vld_map_ms.p99
        present_sync_interval = $metrics.present_sync_interval
        cpu_timing_diagnostic = $true
    }
    $record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
        (Join-Path $out 'trial-summary.json') -Encoding utf8
    $completed = $true
    Write-Host "decoder QoS欠落=$($record.decoder_qos_missing)、OS未表示=$($record.os_not_displayed_interior)/$($record.os_interior_frames)"
} finally {
    if ($null -eq $savedTiming) { Remove-Item Env:PRORES_DX11_CPU_TIMING -ErrorAction SilentlyContinue }
    else { $env:PRORES_DX11_CPU_TIMING = $savedTiming }
    if ($null -eq $savedDebug) { Remove-Item Env:GST_DEBUG -ErrorAction SilentlyContinue }
    else { $env:GST_DEBUG = $savedDebug }
    if (!$completed) {
        if (!$monitor.HasExited) { Stop-Process -Id $monitor.Id -ErrorAction SilentlyContinue }
        & logman stop $session -ets *> $null
    }
}
