# AC境界修正の交互性能比較を、旧/新版の同一ホスト反復として集計する。
[CmdletBinding()]
param(
    [string[]]$InputDirs = @('results/ac-boundary-ab-2026-09-24',
                            'results/ac-boundary-ab-repeat-2026-09-24'),
    [string]$OutFile = 'results/ac-boundary-ab-summary-2026-09-24.json'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
function Median([double[]]$Values) {
    if (!$Values.Count) { throw '中央値を計算できる値がありません' }
    $sorted = @($Values | Sort-Object)
    $mid = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return [double]$sorted[$mid] }
    return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2
}
$rows = @()
for ($set = 0; $set -lt $InputDirs.Count; $set++) {
    $path = Join-Path (Join-Path $root $InputDirs[$set]) 'trial-summary.json'
    if (!(Test-Path -LiteralPath $path)) { throw "試行集計がありません: $path" }
    $items = @(Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
    if ($items.Count -ne 16) { throw "4条件×2版×2組ではありません: $path" }
    foreach ($item in $items) {
        if ($item.variant -notin @('old', 'new') -or
            $item.pair -notin @(0, 1) -or $item.steady_fps -le 0) {
            throw "不正な試行記録: $path"
        }
        $rows += [pscustomobject]@{
            set = $set
            pair = [int]$item.pair
            variant = $item.variant
            input = $item.input
            mode = $item.mode
            steady_fps = [double]$item.steady_fps
            interval_p99_ms = [double]$item.interval_p99_ms
        }
    }
}
$comparison = @()
foreach ($group in ($rows | Group-Object input, mode)) {
    $records = @($group.Group)
    if ($records.Count -ne 8) { throw "条件に8試行がありません: $($group.Name)" }
    $old = @($records | Where-Object variant -EQ old)
    $new = @($records | Where-Object variant -EQ new)
    if ($old.Count -ne 4 -or $new.Count -ne 4) {
        throw "旧/新版の試行数が違います: $($group.Name)"
    }
    $oldMedian = Median @($old.steady_fps)
    $newMedian = Median @($new.steady_fps)
    $paired = @()
    foreach ($set in 0..($InputDirs.Count - 1)) {
        foreach ($pair in 0, 1) {
            $oldRun = @($old | Where-Object { $_.set -eq $set -and $_.pair -eq $pair })
            $newRun = @($new | Where-Object { $_.set -eq $set -and $_.pair -eq $pair })
            if ($oldRun.Count -ne 1 -or $newRun.Count -ne 1) {
                throw "交互ペアが欠けています: $($group.Name) set=$set pair=$pair"
            }
            $paired += [Math]::Round(100 * ($newRun[0].steady_fps / $oldRun[0].steady_fps - 1), 3)
        }
    }
    $comparison += [pscustomobject]@{
        input = $records[0].input
        mode = $records[0].mode
        pairs = $paired.Count
        old_median_fps = [Math]::Round($oldMedian, 3)
        new_median_fps = [Math]::Round($newMedian, 3)
        ratio_of_medians_percent = [Math]::Round(100 * ($newMedian / $oldMedian - 1), 3)
        paired_changes_percent = $paired
        paired_median_change_percent = [Math]::Round((Median $paired), 3)
        old_min_fps = [Math]::Round(($old.steady_fps | Measure-Object -Minimum).Minimum, 3)
        new_min_fps = [Math]::Round(($new.steady_fps | Measure-Object -Minimum).Minimum, 3)
    }
}
[pscustomobject]@{
    status = '正常な合成素材のみのRTX 3070交互比較。実表示・他GPUを保証しない'
    old_head = '2a64823794fc51ecd57e30ee86b6fe9991facda5'
    new_head = 'fdff05220eb7009c14e534e5697d26413c8f2f91'
    inputs = $InputDirs
    measurements = $rows.Count
    comparison = $comparison
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root $OutFile) -Encoding utf8
Write-Host "AC境界修正の交互比較: $($rows.Count)測定、$($comparison.Count)条件を集計"
