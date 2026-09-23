# 標準sinkの処理期限だけを交互変更し、PTS同期・通常QoSの実表示を測る。
[CmdletBinding()]
param(
    [int]$Repeats = 2,
    [int]$Loops = 3,
    [int]$SettleMs = 150,
    [ValidateSet(0, 1)][int]$OrderOffset = 0,
    [switch]$Topmost,
    [string]$OutDir = 'results/display-processing-deadline-2026-09-24',
    [string]$Source = 'media/reference-dji-nature-4k60-rec709-hq.mov'
)
$ErrorActionPreference = 'Stop'
if ($Repeats -lt 1 -or $Loops -lt 1 -or $Loops -gt 10) {
    throw 'Repeatsは1以上、Loopsは1～10を指定してください'
}
if ($SettleMs -lt 0 -or $SettleMs -gt 5000) { throw 'SettleMsは0～5000を指定してください' }
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/vs18'
$presentMon = Join-Path $build 'diagnostic-tools/PresentMon-2.6.0-x64.exe'
$bench = Join-Path $build 'Release/d3d11_display_bench.exe'
$python = (Get-Command python -ErrorAction Stop).Source
$sourcePath = Join-Path $root $Source
$out = Join-Path $root $OutDir
foreach ($required in @($presentMon, $bench, $sourcePath)) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$sessionPrefix = 'PDl' + (Get-Date -Format 'MMddHHmmss')
$records = @()
for ($repeat = 0; $repeat -lt $Repeats; ++$repeat) {
    $conditions = if (($repeat + $OrderOffset) % 2 -eq 0) { @(15, 45) } else { @(45, 15) }
    foreach ($deadline in $conditions) {
        $tag = "r$repeat-pd$deadline"
        $trialDir = Join-Path $out $tag
        $capture = Join-Path $out "$tag-presentmon.csv"
        if ((Test-Path -LiteralPath $trialDir) -or (Test-Path -LiteralPath $capture)) {
            throw "既存の試行出力を上書きしません: $tag"
        }
        New-Item -ItemType Directory -Force -Path $trialDir | Out-Null
        $monitorLog = Join-Path $out "$tag-presentmon.log"
        $monitorError = Join-Path $out "$tag-presentmon-error.log"
        $benchLog = Join-Path $out "$tag-benchmark.log"
        $session = "$sessionPrefix$repeat$deadline"
        $duration = 10 + 8 * $Loops + [int][Math]::Ceiling($SettleMs / 1000.0)
        $monitor = Start-Process -FilePath $presentMon -ArgumentList @(
            '--process_name', 'd3d11_display_bench.exe', '--output_file', $capture,
            '--qpc_time_ms', '--write_display_metadata', '--set_circular_buffer_size', '32768',
            '--timed', [string]$duration, '--terminate_after_timed', '--no_console_stats',
            '--session_name', $session
        ) -WindowStyle Hidden -PassThru -RedirectStandardOutput $monitorLog `
            -RedirectStandardError $monitorError
        $completed = $false
        try {
            Start-Sleep -Milliseconds 1200
            $arguments = @('scripts/benchmark-d3d11-display.py', $sourcePath,
                '--loops', [string]$Loops, '--repeats', '1', '--preroll', '--native-rgb',
                '--trace-sink-return', '--sink-processing-deadline-ms', [string]$deadline,
                '--settle-ms', [string]$SettleMs, '--out', $trialDir)
            if ($Topmost) { $arguments += '--topmost-window' }
            else { $arguments += '--trace-window-state' }
            & $python @arguments *> $benchLog
            if ($LASTEXITCODE -ne 0) { throw "表示試行失敗: $tag ($benchLog)" }
            if (!$monitor.WaitForExit(30000)) { throw "PresentMon終了待ちが時間切れ: $tag" }
            if ($monitor.ExitCode -ne 0 -or !(Test-Path -LiteralPath $capture)) {
                throw "PresentMon取得失敗: $tag ($monitorError)"
            }
            $summary = Join-Path $out "$tag-summary.json"
            & $python 'scripts/summarize-presentmon-display.py' $capture $trialDir '--out' $summary `
                *> (Join-Path $out "$tag-summary.log")
            if ($LASTEXITCODE -ne 0) { throw "PTS/OS表示照合失敗: $tag" }
            $record = Get-Content -LiteralPath $summary -Raw | ConvertFrom-Json
            $trial = Get-ChildItem -LiteralPath $trialDir -Filter '*.json' | Select-Object -First 1
            if (!$trial) { throw "表示ベンチJSONがありません: $tag" }
            $metrics = Get-Content -LiteralPath $trial.FullName -Raw | ConvertFrom-Json
            if ($metrics.sink_processing_deadline_ms -ne $deadline -or
                !$metrics.sink_clock_sync -or $metrics.present_sync_interval -ne 0 -or
                $metrics.decoder_no_qos -or $metrics.lossless_sink_policy -or
                !$metrics.trace_window_state -or
                $metrics.topmost_window -ne [bool]$Topmost -or
                ($Topmost -and !$metrics.topmost_request_ok)) {
                throw "表示条件の記録が不一致: $tag"
            }
            if ($record.coverage_sufficient_trials -ne 1 -or
                $record.pts_aligned_trials -ne 1 -or
                $record.interior_uncaptured_aligned -ne 0 -or
                $metrics.rendered + $metrics.stages.end_to_end_missing -ne
                    $metrics.source_frames * $Loops) {
                throw "出力枚数またはOS表示捕捉が不完全: $tag"
            }
            $records += [ordered]@{
                tag = $tag
                deadline_ms = $deadline
                rendered = $metrics.rendered
                expected_frames = $metrics.source_frames * $Loops
                decoder_qos_missing = $metrics.stages.end_to_end_missing
                sink_dropped = $metrics.dropped
                qos_messages = $metrics.qos_messages
                os_not_displayed_interior = $record.interior_captured_but_not_displayed_aligned
                os_interior_frames = $record.interior_source_frames_aligned
                max_display_gap_ms = $record.max_display_gap_ms
            }
            $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
                (Join-Path $out 'trial-summary.json') -Encoding utf8
            $completed = $true
            Write-Host "$tag`: rendered=$($metrics.rendered)/$($metrics.source_frames * $Loops)、QoS=$($metrics.qos_messages)、OS未表示=$($record.interior_captured_but_not_displayed_aligned)/$($record.interior_source_frames_aligned)、最大間隔=$($record.max_display_gap_ms)ms"
        } finally {
            if (!$completed) {
                if (!$monitor.HasExited) { Stop-Process -Id $monitor.Id -ErrorAction SilentlyContinue }
                & logman stop $session -ets *> $null
            }
            $monitor.Dispose()
        }
        Start-Sleep -Milliseconds 800
    }
}
