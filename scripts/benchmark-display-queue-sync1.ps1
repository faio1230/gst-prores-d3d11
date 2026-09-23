# 診断専用Present(1)下でRGB後queueのQoS欠落とOS未表示を交互比較する。
[CmdletBinding()]
param(
    [ValidateRange(1, 4)][int]$Repeats = 2,
    [ValidateRange(1, 10)][int]$Loops = 3,
    [string]$OutDir = 'results/display-queue-sync1-2026-09-24',
    [string]$Source = 'media/reference-dji-nature-4k60-rec709-hq.mov'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/vs18'
$out = Join-Path $root $OutDir
$sourcePath = Join-Path $root $Source
$presentMon = Join-Path $build 'diagnostic-tools/PresentMon-2.6.0-x64.exe'
$bench = Join-Path $build 'Release/d3d11_display_bench.exe'
$python = (Get-Command python -ErrorAction Stop).Source
if (Test-Path -LiteralPath $out) { throw "既存の結果を上書きしません: $out" }
foreach ($required in @($sourcePath, $presentMon, $bench)) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
$sessionPrefix = 'QS1' + (Get-Date -Format 'MMddHHmmss')
for ($repeat = 0; $repeat -lt $Repeats; $repeat++) {
    $order = if ($repeat % 2 -eq 0) { @(0, 1) } else { @(1, 0) }
    foreach ($queue in $order) {
        $tag = "r$repeat-q$queue"
        $trial = Join-Path $out $tag
        $capture = Join-Path $out "$tag-presentmon.csv"
        New-Item -ItemType Directory -Path $trial | Out-Null
        $session = "$sessionPrefix$repeat$queue"
        $duration = 10 + 8 * $Loops
        $monitor = Start-Process -FilePath $presentMon -ArgumentList @(
            '--process_name', 'd3d11_display_bench.exe', '--output_file', $capture,
            '--qpc_time_ms', '--write_display_metadata', '--set_circular_buffer_size', '32768',
            '--timed', [string]$duration, '--terminate_after_timed', '--no_console_stats',
            '--session_name', $session
        ) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out "$tag-presentmon.log") `
            -RedirectStandardError (Join-Path $out "$tag-presentmon-error.log")
        $completed = $false
        try {
            Start-Sleep -Milliseconds 1200
            $arguments = @('scripts/benchmark-d3d11-display.py', $sourcePath,
                '--loops', [string]$Loops, '--repeats', '1', '--preroll', '--native-rgb',
                '--trace-sink-return', '--trace-window-state', '--present-sync-interval', '1',
                '--settle-ms', '150', '--out', $trial)
            if ($queue) { $arguments += '--queue-after-rgb' }
            & $python @arguments *> (Join-Path $out "$tag-benchmark.log")
            if ($LASTEXITCODE) { throw "表示試行に失敗: $tag" }
            if (!$monitor.WaitForExit(30000) -or $monitor.ExitCode -ne 0 -or
                !(Test-Path -LiteralPath $capture)) { throw "PresentMon取得失敗: $tag" }
            $summary = Join-Path $out "$tag-summary.json"
            & $python 'scripts/summarize-presentmon-display.py' $capture $trial '--out' $summary `
                *> (Join-Path $out "$tag-summary.log")
            if ($LASTEXITCODE) { throw "PTS/OS表示照合失敗: $tag" }
            $record = Get-Content -LiteralPath $summary -Raw | ConvertFrom-Json
            $trialJson = Get-ChildItem -LiteralPath $trial -Filter '*.json' |
                Select-Object -First 1 -ExpandProperty FullName
            if (!$trialJson) { throw "試行JSONがありません: $tag" }
            $metrics = Get-Content -LiteralPath $trialJson -Raw | ConvertFrom-Json
            $expected = $metrics.source_frames * $Loops
            if ($metrics.present_sync_interval -ne 1 -or !$metrics.sink_clock_sync -or
                $metrics.decoder_no_qos -or $metrics.lossless_sink_policy -or
                $metrics.postrgb_queue -ne [bool]$queue -or
                !$metrics.trace_window_state -or $metrics.topmost_window -or
                $metrics.rendered + $metrics.stages.end_to_end_missing -ne $expected -or
                $record.coverage_sufficient_trials -ne 1 -or
                $record.pts_aligned_trials -ne 1 -or
                $record.interior_uncaptured_aligned -ne 0) {
                throw "試行条件またはOS表示捕捉が不完全: $tag"
            }
            $records += [pscustomobject]@{
                tag = $tag
                queue_after_rgb = [bool]$queue
                expected_frames = $expected
                rendered = $metrics.rendered
                decoder_qos_missing = $metrics.stages.end_to_end_missing
                sink_dropped = $metrics.dropped
                qos_messages = $metrics.qos_messages
                os_not_displayed_interior = $record.interior_captured_but_not_displayed_aligned
                os_interior_frames = $record.interior_source_frames_aligned
                max_display_gap_ms = $record.max_display_gap_ms
                max_sink_push_ms = $metrics.stages.stage_latency.sink_push_to_sink_return_ms.max
                max_queue_wait_ms = $metrics.stages.stage_latency.rgb_to_rgb_dequeued_ms.max
            }
            $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath `
                (Join-Path $out 'trial-summary.json') -Encoding utf8
            $completed = $true
            Write-Host "$tag`: decoded=$($metrics.rendered)/$expected、QoS=$($metrics.qos_messages)、OS未表示=$($record.interior_captured_but_not_displayed_aligned)/$($record.interior_source_frames_aligned)"
        } finally {
            if (!$completed) {
                if (!$monitor.HasExited) { Stop-Process -Id $monitor.Id -ErrorAction SilentlyContinue }
                & logman stop $session -ets *> $null
            }
        }
    }
}
