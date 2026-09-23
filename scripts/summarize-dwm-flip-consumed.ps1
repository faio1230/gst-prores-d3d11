# 標準d3d11videosinkのPresentを、DWMのWindowed_Dx_Flip_ConsumedとPresentMonへ帰属する。
# presentCountとPresentMon行のずれは、同じswap chainのDXGI開始時刻から校正した値を明示する。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DxgiEtl,
    [Parameter(Mandatory)][string]$DwmEtl,
    [Parameter(Mandatory)][string]$PresentMonCsv,
    [Parameter(Mandatory)][int[]]$ProcessIds,
    [Parameter(Mandatory)][int]$PresentCountOffset,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($DxgiEtl, $DwmEtl, $PresentMonCsv)) {
    if (!(Test-Path -LiteralPath $path)) { throw "入力がありません: $path" }
}
if ($PresentCountOffset -lt 0) { throw 'presentCountのずれは0以上で指定してください' }
$dxgiAll = @(Get-WinEvent -FilterHashtable @{Path = $DxgiEtl; Id = 42} -Oldest)
$dwmAll = @(Get-WinEvent -FilterHashtable @{Path = $DwmEtl; Id = 467} -Oldest)
$pmAll = @(Import-Csv -LiteralPath $PresentMonCsv)
$trials = foreach ($processId in $ProcessIds) {
    $dxgi = @($dxgiAll | Where-Object { $_.ProcessId -eq $processId })
    $pm = @($pmAll | Where-Object { [int]$_.ProcessID -eq $processId })
    if ($dxgi.Count -lt 3 -or !$pm) { throw "DXGIまたはPresentMonが不足: PID=$processId" }
    $first = $dxgi[0].TimeCreated.AddMilliseconds(-100)
    $last = $dxgi[-1].TimeCreated.AddMilliseconds(100)
    $byWindow = @($dwmAll | Where-Object {
        $_.TimeCreated -ge $first -and $_.TimeCreated -le $last
    } | Group-Object { [string]$_.Properties[0].Value } | Sort-Object Count -Descending)
    if (!$byWindow -or $byWindow[0].Count -lt $pm.Count / 2) {
        throw "対象windowのDWM flipイベントを特定できません: PID=$processId"
    }
    $window = $byWindow[0]
    $flips = @($window.Group | Sort-Object TimeCreated)
    $counts = [Collections.Generic.HashSet[int]]::new()
    $lags = @()
    foreach ($flip in $flips) {
        if ($flip.Properties.Count -lt 9) { throw "DWM ID467の形式が異なります: PID=$processId" }
        $count = [int]$flip.Properties[8].Value
        if (!$counts.Add($count)) { throw "DWM presentCountが重複: PID=$processId count=$count" }
        $index = $count - $PresentCountOffset
        if ($index -ge 0 -and $index -lt $dxgi.Count) {
            $lag = ($flip.TimeCreated - $dxgi[$index].TimeCreated).TotalMilliseconds
            if ($lag -lt 0 -or $lag -gt 100) {
                throw "DWMとDXGIの順序・時刻が矛盾: PID=$processId count=$count lag=$lag"
            }
            $lags += $lag
        }
    }
    $sortedCounts = @($counts | Sort-Object)
    $minCount = $sortedCounts[0]
    $maxCount = $sortedCounts[-1]
    $expectedRows = $maxCount - $PresentCountOffset + 1
    $coverageSufficient = $minCount -eq $PresentCountOffset -and
        $pm.Count -eq $expectedRows
    $drifts = @()
    if ($coverageSufficient) {
        # 先頭のwindow初期presentだけ時刻の基準が揺れるため、2行目を相対時刻の基準にする。
        $baseDxgi = $dxgi[1].TimeCreated
        $basePm = [double]$pm[1].CPUStartQPCTimeInMs
        for ($i = 1; $i -lt $pm.Count; $i++) {
            $drifts += [Math]::Abs(($dxgi[$i].TimeCreated - $baseDxgi).TotalMilliseconds -
                ([double]$pm[$i].CPUStartQPCTimeInMs - $basePm))
        }
        # GStreamer sinkの実写反復では稀に1～2msのETW/QPCずれがある。
        # 件数と順序を保ち、2ms超なら誤帰属として中止する。
        if (($drifts | Measure-Object -Maximum).Maximum -gt 2) {
            throw "DXGIとPresentMonの相対時刻が2ms超: PID=$processId"
        }
    }
    $mismatches = @()
    $missing = 0
    if ($coverageSufficient) {
        for ($i = 0; $i -lt $pm.Count; $i++) {
            $consumed = $counts.Contains($i + $PresentCountOffset)
            $displayed = $pm[$i].MsUntilDisplayed -ne 'NA'
            if (!$consumed) { $missing++ }
            if ($consumed -ne $displayed) { $mismatches += $i }
        }
    }
    $sortedLags = @($lags | Sort-Object)
    [ordered]@{
        process_id = $processId
        dxgi_present_starts = $dxgi.Count
        presentmon_rows = $pm.Count
        presentmon_without_display_time = @($pm | Where-Object MsUntilDisplayed -eq 'NA').Count
        dwm_window_handle = $window.Name
        dwm_flip_consumed = $flips.Count
        dwm_present_count_min = $minCount
        dwm_present_count_max = $maxCount
        present_count_offset = $PresentCountOffset
        coverage_sufficient = $coverageSufficient
        dwm_missing_present_counts = if ($coverageSufficient) { $missing } else { $null }
        presentmon_dwm_status_agreement = if ($coverageSufficient) { $pm.Count - $mismatches.Count } else { $null }
        mismatch_indices = $mismatches
        max_dxgi_presentmon_relative_drift_ms = if ($coverageSufficient) {
            ($drifts | Measure-Object -Maximum).Maximum
        } else { $null }
        dwm_consumption_lag_ms = [ordered]@{
            min = $sortedLags[0]
            median = $sortedLags[[int]($sortedLags.Count * 0.5)]
            p99 = $sortedLags[[int]($sortedLags.Count * 0.99)]
            max = $sortedLags[-1]
        }
    }
}
$result = [ordered]@{
    method = 'DXGI ID42のPID・相対QPC、DWM-Core ID467のHWND/presentCount、PresentMonの同一PID行を照合'
    dxgi_sha256 = (Get-FileHash -LiteralPath $DxgiEtl -Algorithm SHA256).Hash.ToLowerInvariant()
    dwm_sha256 = (Get-FileHash -LiteralPath $DwmEtl -Algorithm SHA256).Hash.ToLowerInvariant()
    presentmon_sha256 = (Get-FileHash -LiteralPath $PresentMonCsv -Algorithm SHA256).Hash.ToLowerInvariant()
    trials = @($trials)
    caveat = 'DWM consumed欠番とOS未表示の一致は、破棄位置・原因を単独で証明しない。捕捉不足の試行は一致率を算出しない。'
}
$directory = Split-Path -Parent $Out
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Out -Encoding utf8
foreach ($trial in $trials) {
    Write-Output ("PID={0} PM={1} DWM consumed={2} 欠番={3} OS未表示={4} 一致={5} 捕捉十分={6}" -f
        $trial.process_id, $trial.presentmon_rows, $trial.dwm_flip_consumed,
        $trial.dwm_missing_present_counts, $trial.presentmon_without_display_time,
        $trial.presentmon_dwm_status_agreement, $trial.coverage_sufficient)
}
