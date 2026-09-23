# Win32Kのtoken状態を、既にPTS帰属したDWM flip消費欠番へ対応づける。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Win32kEtl,
    [Parameter(Mandatory)][string]$DwmEtl,
    [Parameter(Mandatory)][string]$DwmSummary,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
foreach ($path in @($Win32kEtl, $DwmEtl, $DwmSummary)) {
    if (!(Test-Path -LiteralPath $path)) { throw "入力がありません: $path" }
}
function Measure-Values([double[]]$values) {
    if (!$values.Count) { return $null }
    $sorted = @($values | Sort-Object)
    return [ordered]@{
        count = $sorted.Count
        min_ms = $sorted[0]
        median_ms = $sorted[[int][Math]::Floor($sorted.Count * 0.5)]
        p99_ms = $sorted[[int][Math]::Floor($sorted.Count * 0.99)]
        max_ms = $sorted[-1]
    }
}
$summary = Get-Content -LiteralPath $DwmSummary -Raw | ConvertFrom-Json
$createdAll = @(Get-WinEvent -FilterHashtable @{Path = $Win32kEtl; Id = 201} -Oldest)
$statesAll = @(Get-WinEvent -FilterHashtable @{Path = $Win32kEtl; Id = 301} -Oldest)
$flipsAll = @(Get-WinEvent -FilterHashtable @{Path = $DwmEtl; Id = 467} -Oldest)
$trials = foreach ($trial in $summary.trials) {
    if (!$trial.coverage_sufficient -or $trial.mismatch_indices.Count) {
        throw "DWMとPresentMonのPTS帰属が不完全: PID=$($trial.process_id)"
    }
    $flips = @($flipsAll | Where-Object {
        [string]$_.Properties[0].Value -eq $trial.dwm_window_handle
    })
    if ($flips.Count -ne $trial.dwm_flip_consumed) {
        throw "DWM flip件数が変化: PID=$($trial.process_id)"
    }
    $surface = [uint64]$flips[0].Properties[6].Value
    $bind = [uint64]$flips[0].Properties[7].Value
    $consumed = [Collections.Generic.HashSet[int]]::new()
    foreach ($flip in $flips) {
        if ([uint64]$flip.Properties[6].Value -ne $surface -or
            [uint64]$flip.Properties[7].Value -ne $bind) {
            throw "同一windowのsurface/bindが変化: PID=$($trial.process_id)"
        }
        [void]$consumed.Add([int]$flip.Properties[8].Value)
    }
    $created = @($createdAll | Where-Object {
        [uint64]$_.Properties[4].Value -eq $surface -and
        [uint64]$_.Properties[5].Value -eq $bind
    })
    $createdTime = @{}
    foreach ($event in $created) {
        $count = [int]$event.Properties[3].Value
        if ($createdTime.ContainsKey($count)) { throw "重複token作成: count=$count" }
        $createdTime[$count] = $event.TimeCreated
    }
    $states = @($statesAll | Where-Object {
        [uint64]$_.Properties[7].Value -eq $surface -and
        [uint64]$_.Properties[8].Value -eq $bind
    })
    $byCount = @{}
    foreach ($event in $states) {
        $count = [int]$event.Properties[2].Value
        if (!$byCount.ContainsKey($count)) {
            $byCount[$count] = [Collections.Generic.List[object]]::new()
        }
        $byCount[$count].Add($event)
    }
    $inFrame = @{}
    $sequences = @{}
    $sequenceByCount = @{}
    $sequenceByConsumption = @{ consumed = @{}; missing = @{} }
    $independent = 0
    $early = 0
    $first = [int]$trial.dwm_present_count_min
    $last = [int]$trial.dwm_present_count_max
    for ($count = $first; $count -le $last; $count++) {
        if (!$byCount.ContainsKey($count)) { throw "Win32K状態が不足: count=$count" }
        $ordered = @($byCount[$count] | Sort-Object TimeCreated)
        $sequence = (@($ordered | ForEach-Object { [int]$_.Properties[4].Value }) -join ',')
        if (!$sequences.ContainsKey($sequence)) { $sequences[$sequence] = 0 }
        $sequences[$sequence]++
        $sequenceByCount[$count] = $sequence
        $class = if ($consumed.Contains($count)) { 'consumed' } else { 'missing' }
        if (!$sequenceByConsumption[$class].ContainsKey($sequence)) {
            $sequenceByConsumption[$class][$sequence] = 0
        }
        $sequenceByConsumption[$class][$sequence]++
        $inFrames = @($ordered | Where-Object { $_.Properties[4].Value -eq 3 })
        if ($inFrames.Count -ne 1) { throw "InFrame状態が一意ではない: count=$count" }
        $inFrame[$count] = $inFrames[0].TimeCreated
        $independent += @($ordered | Where-Object { $_.Properties[5].Value -eq $true }).Count
        $early += @($ordered | Where-Object { $_.Properties[9].Value -eq $true }).Count
    }
    $missing = @()
    $nextConsumedDeltas = @()
    $missingDeltasBySequence = @{}
    $nextConsumedUnavailable = 0
    $adjacentConsumedDeltas = @()
    $missingNextCreatedDeltas = @{ with_retired = @(); without_retired = @() }
    $missingNextCreatedBefore = @{ with_retired = 0; without_retired = 0 }
    $adjacentConsumedNextCreatedDeltas = @()
    $adjacentConsumedNextCreatedBefore = 0
    $missingNextTokenOrder = @()
    for ($count = $first; $count -le $last; $count++) {
        if (!$createdTime.ContainsKey($count + 1)) {
            throw "次のtoken作成時刻が不足: count=$count"
        }
        $nextCreatedDelta = ($createdTime[$count + 1] - $inFrame[$count]).TotalMilliseconds
        if (!$consumed.Contains($count)) {
            $missing += $count
            $sequence = $sequenceByCount[$count]
            $class = if ($sequence -match '(^|,)5(,|$)') { 'with_retired' } else { 'without_retired' }
            $missingNextCreatedDeltas[$class] += $nextCreatedDelta
            if ($nextCreatedDelta -le 0) { $missingNextCreatedBefore[$class]++ }
            $missingNextTokenOrder += [ordered]@{
                present_count = $count
                state_sequence = $sequence
                next_token_created_minus_inframe_ms = $nextCreatedDelta
            }
            $next = $count + 1
            while ($next -le $last -and !$consumed.Contains($next)) { $next++ }
            if ($next -le $last) {
                $delta = ($inFrame[$next] - $inFrame[$count]).TotalMilliseconds
                $nextConsumedDeltas += $delta
                $sequence = $sequenceByCount[$count]
                if (!$missingDeltasBySequence.ContainsKey($sequence)) {
                    $missingDeltasBySequence[$sequence] = @()
                }
                $missingDeltasBySequence[$sequence] += $delta
            } else {
                $nextConsumedUnavailable++
            }
        } elseif ($count -lt $last -and $consumed.Contains($count + 1)) {
            $adjacentConsumedDeltas += ($inFrame[$count + 1] - $inFrame[$count]).TotalMilliseconds
            $adjacentConsumedNextCreatedDeltas += $nextCreatedDelta
            if ($nextCreatedDelta -le 0) { $adjacentConsumedNextCreatedBefore++ }
        }
    }
    if ($missing.Count -ne $trial.dwm_missing_present_counts) {
        throw "DWM欠番数が集計と異なる: PID=$($trial.process_id)"
    }
    $createdCounts = @($created | ForEach-Object { [int]$_.Properties[3].Value })
    $missingSequenceTimings = @{}
    foreach ($sequence in $missingDeltasBySequence.Keys) {
        $missingSequenceTimings[$sequence] = Measure-Values $missingDeltasBySequence[$sequence]
    }
    [ordered]@{
        process_id = $trial.process_id
        window_handle = $trial.dwm_window_handle
        composition_surface_luid = [string]$surface
        bind_id = [string]$bind
        token_created = $created.Count
        token_created_count_min = ($createdCounts | Measure-Object -Minimum).Minimum
        token_created_count_max = ($createdCounts | Measure-Object -Maximum).Maximum
        analyzed_present_counts = $last - $first + 1
        state_sequence_counts = $sequences
        state_sequence_by_consumption = $sequenceByConsumption
        independent_flip_true_events = $independent
        early_composition_true_events = $early
        dwm_missing_present_counts = $missing
        missing_next_consumed_unavailable = $nextConsumedUnavailable
        missing_next_consumed_inframe_delta = Measure-Values $nextConsumedDeltas
        missing_inframe_delta_by_sequence = $missingSequenceTimings
        missing_next_consumed_within_0_1_ms = @($nextConsumedDeltas | Where-Object { $_ -ge 0 -and $_ -le 0.1 }).Count
        missing_next_token_created_minus_inframe_ms = [ordered]@{
            with_retired = Measure-Values $missingNextCreatedDeltas.with_retired
            without_retired = Measure-Values $missingNextCreatedDeltas.without_retired
        }
        missing_next_token_created_before_inframe = $missingNextCreatedBefore
        missing_next_token_order = $missingNextTokenOrder
        adjacent_consumed_inframe_delta = Measure-Values $adjacentConsumedDeltas
        adjacent_consumed_within_0_1_ms = @($adjacentConsumedDeltas | Where-Object { $_ -ge 0 -and $_ -le 0.1 }).Count
        adjacent_consumed_next_token_created_minus_inframe_ms = Measure-Values $adjacentConsumedNextCreatedDeltas
        adjacent_consumed_next_token_created_before_inframe = $adjacentConsumedNextCreatedBefore
    }
}
$result = [ordered]@{
    method = 'DWM ID467のsurfaceLuid/bindId/presentCountをWin32K ID201/301へ結合。NewState=3の時刻を次の消費tokenおよび直後tokenの作成時刻と比較。'
    win32k_sha256 = (Get-FileHash -LiteralPath $Win32kEtl -Algorithm SHA256).Hash.ToLowerInvariant()
    dwm_sha256 = (Get-FileHash -LiteralPath $DwmEtl -Algorithm SHA256).Hash.ToLowerInvariant()
    dwm_summary_sha256 = (Get-FileHash -LiteralPath $DwmSummary -Algorithm SHA256).Hash.ToLowerInvariant()
    token_states = [ordered]@{completed = 2; in_frame = 3; confirmed = 4; retired = 5; discarded = 6}
    trials = @($trials)
    caveat = 'state 6は表示済みtokenにも出る後片付け状態。直後tokenの先行作成は追い越し可能な時系列を示すが、キューでの置換主体や正確な破棄位置を単独では証明しない。'
}
$directory = Split-Path -Parent $Out
if ($directory) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Out -Encoding utf8
foreach ($trial in $trials) {
    Write-Output ("PID={0} 未消費={1} 次の消費tokenとInFrame 0.1ms内={2} 比較対照0.1ms内={3}/{4} 直後token先行={5}/{6} 対照={7}/{4}" -f
        $trial.process_id, $trial.dwm_missing_present_counts.Count,
        $trial.missing_next_consumed_within_0_1_ms,
        $trial.adjacent_consumed_within_0_1_ms,
        $trial.adjacent_consumed_inframe_delta.count,
        $trial.missing_next_token_created_before_inframe.with_retired,
        $trial.missing_next_token_created_minus_inframe_ms.with_retired.count,
        $trial.adjacent_consumed_next_token_created_before_inframe)
}
