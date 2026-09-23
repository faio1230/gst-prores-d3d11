# DxgKrnl PresentHistory開始／DWM handoffをWin32K tokenへ1対1で帰属する。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DxgKrnlEtl,
    [Parameter(Mandatory)][string]$Win32kEtl,
    [Parameter(Mandatory)][string]$Win32kSummary,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($DxgKrnlEtl, $Win32kEtl, $Win32kSummary)) {
    if (!(Test-Path -LiteralPath $path)) { throw "入力がありません: $path" }
}

function Measure-Values([double[]]$values) {
    if (!$values.Count) { return $null }
    $sorted = @($values | Sort-Object)
    return [ordered]@{
        count = $sorted.Count
        min_ms = $sorted[0]
        median_ms = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.5)]
        p95_ms = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.95)]
        p99_ms = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.99)]
        max_ms = $sorted[-1]
    }
}

# Windows 11の本ETLではID215=PresentHistoryDetailed_Start、ID172=PresentHistory_Info。
# ID215は同じToken pointerを再利用するため、次のStartまでの最初のInfoに限る。
$summary = Get-Content -LiteralPath $Win32kSummary -Raw | ConvertFrom-Json
$win32kHash = (Get-FileHash -LiteralPath $Win32kEtl -Algorithm SHA256).Hash.ToLowerInvariant()
if ($win32kHash -ne ([string]$summary.win32k_sha256).ToLowerInvariant()) {
    throw 'Win32K ETLのSHA256が既存token集計と一致しません'
}
$startsAll = @(Get-WinEvent -FilterHashtable @{ Path = $DxgKrnlEtl; Id = 215 } -Oldest)
$infosAll = @(Get-WinEvent -FilterHashtable @{ Path = $DxgKrnlEtl; Id = 172 } -Oldest)
$createdAll = @(Get-WinEvent -FilterHashtable @{ Path = $Win32kEtl; Id = 201 } -Oldest)
$statesAll = @(Get-WinEvent -FilterHashtable @{ Path = $Win32kEtl; Id = 301 } -Oldest)

$startsByToken = @{}
foreach ($event in $startsAll) {
    $token = [string][uint64]$event.Properties[1].Value
    if (!$startsByToken.ContainsKey($token)) {
        $startsByToken[$token] = [Collections.Generic.List[object]]::new()
    }
    $startsByToken[$token].Add($event)
}
foreach ($token in @($startsByToken.Keys)) {
    $startsByToken[$token] = @($startsByToken[$token] | Sort-Object TimeCreated, RecordId)
}
$nextStartByRecord = @{}
foreach ($list in $startsByToken.Values) {
    for ($index = 0; $index -lt $list.Count - 1; $index++) {
        $nextStartByRecord[[string]$list[$index].RecordId] = $list[$index + 1].TimeCreated
    }
}
$infosByToken = @{}
foreach ($event in $infosAll) {
    $token = [string][uint64]$event.Properties[1].Value
    if (!$infosByToken.ContainsKey($token)) {
        $infosByToken[$token] = [Collections.Generic.List[object]]::new()
    }
    $infosByToken[$token].Add($event)
}
foreach ($token in @($infosByToken.Keys)) {
    $infosByToken[$token] = @($infosByToken[$token] | Sort-Object TimeCreated, RecordId)
}

function Find-FirstInfo($start) {
    $token = [string][uint64]$start.Properties[1].Value
    if (!$infosByToken.ContainsKey($token)) { return $null }
    $list = $infosByToken[$token]
    $low = 0
    $high = $list.Count
    while ($low -lt $high) {
        $middle = [int][Math]::Floor(($low + $high) / 2)
        if ($list[$middle].TimeCreated -lt $start.TimeCreated) {
            $low = $middle + 1
        } else {
            $high = $middle
        }
    }
    if ($low -eq $list.Count) { return $null }
    $candidate = $list[$low]
    $nextKey = [string]$start.RecordId
    if ($nextStartByRecord.ContainsKey($nextKey) -and
        $candidate.TimeCreated -ge $nextStartByRecord[$nextKey]) { return $null }
    return $candidate
}

$trials = foreach ($trial in $summary.trials) {
    $trialProcessId = [int]$trial.process_id
    $surface = [uint64]$trial.composition_surface_luid
    $bind = [uint64]$trial.bind_id
    $created = @( $createdAll | Where-Object {
        $_.ProcessId -eq $trialProcessId -and
        [uint64]$_.Properties[4].Value -eq $surface -and
        [uint64]$_.Properties[5].Value -eq $bind
    } )
    $starts = @( $startsAll | Where-Object {
        $_.ProcessId -eq $trialProcessId -and [int]$_.Properties[2].Value -eq 2
    } )
    if ($created.Count -ne [int]$trial.token_created -or $starts.Count -ne $created.Count) {
        throw "DxgKrnl/Win32K開始件数不一致: PID=$trialProcessId dxg=$($starts.Count) win=$($created.Count)"
    }
    $missing = [Collections.Generic.HashSet[int]]::new()
    foreach ($count in $trial.dwm_missing_present_counts) { [void]$missing.Add([int]$count) }
    $inFrame = @{}
    $retired = [Collections.Generic.HashSet[int]]::new()
    foreach ($event in $statesAll) {
        if ([uint64]$event.Properties[7].Value -ne $surface -or
            [uint64]$event.Properties[8].Value -ne $bind) { continue }
        $count = [int]$event.Properties[2].Value
        $state = [int]$event.Properties[4].Value
        if ($state -eq 3) {
            if ($inFrame.ContainsKey($count)) { throw "重複InFrame: PID=$trialProcessId count=$count" }
            $inFrame[$count] = $event.TimeCreated
        }
        if ($state -eq 5) { [void]$retired.Add($count) }
    }
    $startPairDeltas = @()
    $readyDelay = @{ other_including_boundary = @(); missing = @() }
    $readyToInFrame = @{ other_including_boundary = @(); missing = @() }
    $missingReadyToInFrameBySequence = @{ with_retired = @(); without_retired = @() }
    $readyCounts = @{ other_including_boundary = 0; missing = 0 }
    $readyBeforeInFrame = @{ other_including_boundary = 0; missing = 0 }
    $missingBySequence = @{ with_retired = 0; without_retired = 0 }
    $missingExamples = @()
    for ($index = 0; $index -lt $starts.Count; $index++) {
        $start = $starts[$index]
        $win = $created[$index]
        $count = [int]$win.Properties[3].Value
        $delta = ($start.TimeCreated - $win.TimeCreated).TotalMilliseconds
        if ($start.ThreadId -ne $win.ThreadId -or $delta -lt 0 -or $delta -gt 1) {
            throw "DxgKrnl/Win32K順序・スレッド不一致: PID=$trialProcessId count=$count delta=$delta"
        }
        $startPairDeltas += $delta
        if (!$inFrame.ContainsKey($count) -and
            $count -ge [int]$trial.token_created_count_min -and
            $count -le [int]$trial.token_created_count_max) {
            throw "InFrame不足: PID=$trialProcessId count=$count"
        }
        $class = if ($missing.Contains($count)) { 'missing' } else { 'other_including_boundary' }
        if ($class -eq 'missing') {
            if ($retired.Contains($count)) { $missingBySequence.with_retired++ }
            else { $missingBySequence.without_retired++ }
        }
        $info = Find-FirstInfo $start
        if ($info) {
            $readyCounts[$class]++
            $readyDelay[$class] += ($info.TimeCreated - $start.TimeCreated).TotalMilliseconds
            if ($inFrame.ContainsKey($count)) {
                $gap = ($inFrame[$count] - $info.TimeCreated).TotalMilliseconds
                $readyToInFrame[$class] += $gap
                if ($class -eq 'missing') {
                    $sequence = if ($retired.Contains($count)) { 'with_retired' } else { 'without_retired' }
                    $missingReadyToInFrameBySequence[$sequence] += $gap
                }
                if ($gap -ge 0) { $readyBeforeInFrame[$class]++ }
            }
        }
        if ($class -eq 'missing' -and $missingExamples.Count -lt 16) {
            $missingExamples += [ordered]@{
                present_count = $count
                has_retired = $retired.Contains($count)
                start_to_ready_ms = if ($info) { ($info.TimeCreated - $start.TimeCreated).TotalMilliseconds } else { $null }
                ready_to_inframe_ms = if ($info -and $inFrame.ContainsKey($count)) {
                    ($inFrame[$count] - $info.TimeCreated).TotalMilliseconds
                } else { $null }
            }
        }
    }
    if ($readyCounts.missing -ne $missing.Count) {
        throw "未表示のDxgKrnl ready不足: PID=$trialProcessId $($readyCounts.missing)/$($missing.Count)"
    }
    if ($readyCounts.missing + $readyCounts.other_including_boundary -ne $starts.Count) {
        throw "DxgKrnl ready不足: PID=$trialProcessId"
    }
    [ordered]@{
        process_id = $trialProcessId
        created_and_start_paired = $starts.Count
        missing_dwm = $missing.Count
        start_pair_delta_ms = Measure-Values $startPairDeltas
        ready_count = $readyCounts
        ready_before_inframe_count = $readyBeforeInFrame
        start_to_ready_ms = [ordered]@{
            other_including_boundary = Measure-Values $readyDelay.other_including_boundary
            missing = Measure-Values $readyDelay.missing
        }
        ready_to_inframe_ms = [ordered]@{
            other_including_boundary = Measure-Values $readyToInFrame.other_including_boundary
            missing = Measure-Values $readyToInFrame.missing
        }
        missing_by_sequence = $missingBySequence
        missing_ready_to_inframe_by_sequence_ms = [ordered]@{
            with_retired = Measure-Values $missingReadyToInFrameBySequence.with_retired
            without_retired = Measure-Values $missingReadyToInFrameBySequence.without_retired
        }
        missing_examples = $missingExamples
    }
}

$result = [ordered]@{
    method = '同一PID・スレッド・順序・1ms以内でWin32K ID201とDxgKrnl ID215を対応。Tokenの次のStartより前の最初のID172をreadyとして帰属。'
    dxgkrnl_sha256 = (Get-FileHash -LiteralPath $DxgKrnlEtl -Algorithm SHA256).Hash.ToLowerInvariant()
    win32k_summary_sha256 = (Get-FileHash -LiteralPath $Win32kSummary -Algorithm SHA256).Hash.ToLowerInvariant()
    dxgkrnl_start_events = $startsAll.Count
    dxgkrnl_info_events = $infosAll.Count
    trials = @($trials)
    caveat = 'ID172はDWM handoff可能なready時点であって実表示ではない。Token pointerは再利用される。未表示の正確な破棄主体・キュー位置を本集計だけで断定しない。'
}
$parent = Split-Path -Parent $Out
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Out -Encoding utf8
$result.trials | ForEach-Object {
    "PID=$($_.process_id) paired=$($_.created_and_start_paired) missing=$($_.missing_dwm) ready_missing=$($_.ready_count.missing) ready_before_inframe=$($_.ready_before_inframe_count.missing)"
}
