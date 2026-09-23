# DXGI ETWのPresent1開始/終了を、表示ベンチのpresent通知と順序・時刻で照合する。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$EtlPath,
    [Parameter(Mandatory)][string]$BenchDir,
    [Parameter(Mandatory)][string]$PresentMonSummary,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($EtlPath, $BenchDir, $PresentMonSummary)) {
    if (!(Test-Path -LiteralPath $path)) { throw "入力がありません: $path" }
}
$events = @(Get-WinEvent -Path $EtlPath -Oldest |
    Where-Object { $_.ProviderName -eq 'Microsoft-Windows-DXGI' -and $_.Id -in @(42, 43) })
$summary = Get-Content -LiteralPath $PresentMonSummary -Raw | ConvertFrom-Json
$paths = @(Get-ChildItem -LiteralPath $BenchDir -Filter '*.json' |
    Where-Object { $_.BaseName -match '-\d+$' } | Sort-Object Name)
if (!$paths) { throw '表示ベンチの試行JSONがありません' }
$trials = foreach ($path in $paths) {
    $record = Get-Content -LiteralPath $path.FullName -Raw | ConvertFrom-Json
    $processId = [int]$record.process_id
    $starts = @($events | Where-Object { $_.ProcessId -eq $processId -and $_.Id -eq 42 })
    $stops = @($events | Where-Object { $_.ProcessId -eq $processId -and $_.Id -eq 43 })
    $signalPath = Join-Path $path.DirectoryName ($path.BaseName + '.present.csv')
    $signals = @(Import-Csv -LiteralPath $signalPath)
    if ($starts.Count -ne $signals.Count -or $stops.Count -ne $signals.Count -or
        $signals.Count -ne [int]$record.present_count) {
        throw "DXGI/通知件数が一致しません: $($path.Name)"
    }
    $baseEvent = $starts[0].TimeCreated
    $baseSignalMs = [double]$signals[0].wall_ms
    $drifts = @()
    $callMs = @()
    for ($index = 0; $index -lt $signals.Count; $index++) {
        if ($stops[$index].TimeCreated -lt $starts[$index].TimeCreated) {
            throw "DXGI終了が開始より早い: $($path.Name) $index"
        }
        $eventDeltaMs = ($starts[$index].TimeCreated - $baseEvent).TotalMilliseconds
        $signalDeltaMs = [double]$signals[$index].wall_ms - $baseSignalMs
        $drifts += [Math]::Abs($eventDeltaMs - $signalDeltaMs)
        $callMs += ($stops[$index].TimeCreated - $starts[$index].TimeCreated).TotalMilliseconds
    }
    $maxDrift = ($drifts | Measure-Object -Maximum).Maximum
    if ($maxDrift -gt 1.0) { throw "DXGI/通知の相対時刻差が1ms超: $($path.Name) $maxDrift" }
    $pm = @($summary.trial_details | Where-Object { $_.repeat -eq $record.repeat })
    if ($pm.Count -ne 1 -or [int]$pm[0].process_id -ne $processId) {
        throw "PresentMon試行が対応しません: $($path.Name)"
    }
    [ordered]@{
        repeat = $record.repeat
        process_id = $processId
        source_mode = $record.source_mode
        present_signals = $signals.Count
        dxgi_present_starts = $starts.Count
        dxgi_present_stops = $stops.Count
        dxgi_sync_intervals = @($starts | ForEach-Object { $_.Properties[2].Value } |
            Sort-Object -Unique)
        dxgi_flags = @($starts | ForEach-Object { $_.Properties[1].Value } |
            Sort-Object -Unique)
        dxgi_results = @($stops | ForEach-Object { $_.Properties[0].Value } |
            Sort-Object -Unique)
        dxgi_swap_chains = @($starts | ForEach-Object { $_.Properties[0].Value } |
            Sort-Object -Unique).Count
        max_signal_to_dxgi_relative_drift_ms = $maxDrift
        max_dxgi_call_ms = ($callMs | Measure-Object -Maximum).Maximum
        presentmon_coverage_sufficient = $pm[0].presentmon_coverage_sufficient
        interior_captured_but_not_displayed =
            $pm[0].pts_alignment.interior_captured_but_not_displayed
        interior_uncaptured = $pm[0].pts_alignment.interior_uncaptured
    }
}
$result = [ordered]@{
    method = '同一PIDのDXGI ETW ID42/43とpresent通知を順番・相対時刻1ms以内で照合'
    etl_path = (Resolve-Path -LiteralPath $EtlPath).Path
    etl_sha256 = (Get-FileHash -LiteralPath $EtlPath -Algorithm SHA256).Hash.ToLowerInvariant()
    trials = @($trials)
    caveat = 'DXGI呼び出し成功はDWMのキュー投入・表示を証明しない。OS表示はPresentMonを別途PTS照合する。'
}
$directory = Split-Path -Parent $Out
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Out -Encoding utf8
foreach ($trial in $trials) {
    Write-Output ("repeat={0} pid={1} Present={2} result={3} coverage={4} OS未表示={5} 最大相対ずれ={6:N3}ms" -f
        $trial.repeat, $trial.process_id, $trial.present_signals,
        ($trial.dxgi_results -join ','), $trial.presentmon_coverage_sufficient,
        $trial.interior_captured_but_not_displayed,
        $trial.max_signal_to_dxgi_relative_drift_ms)
}
