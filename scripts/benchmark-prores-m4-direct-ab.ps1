# M3採用版とM4作業版のHQ directを同一GPU・交互4組で比較する。
[CmdletBinding()]
param([string]$OutDir = 'results/m4-hq-direct-ab-2026-09-24',
      [string]$NewBuildName = 'prores-m4-direct-ab-new',
      [string]$OldBuildName = 'prores-m3-direct-ab-new',
      [string]$PriorSummary = 'results/m3-alpha-hq-direct-ab-2026-09-24/summary.json')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/vs18'
$oldBuild = Join-Path $build $OldBuildName
$newBuild = Join-Path $build $NewBuildName
$out = Join-Path $root $OutDir
$files = @('gstproresd3d11.dll','prores_vld.cso','prores_idct_unorm.cso',
    'prores_rgb.cso','prores_alpha.cso','prores_pack_alpha.cso','prores_rgb_alpha.cso')
$sources = @((Join-Path $root 'media/synthetic-1080p60-hq.mov'),
             (Join-Path $root 'media/synthetic-2160p60-hq.mov'))
$prior = Get-Content (Join-Path $root $PriorSummary) -Raw |
    ConvertFrom-Json
if ((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath $newBuild)) {
    throw '既存M4測定結果・配置は上書きしません'
}
$oldHash = (Get-FileHash -LiteralPath (Join-Path $oldBuild 'plugins/Release/gstproresd3d11.dll') `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($oldHash -ne $prior.new_plugin_sha256) { throw '旧採用版のSHA256が不一致' }
$newPlugin = Join-Path $newBuild 'plugins/Release'
$newExe = Join-Path $newBuild 'Release'
New-Item -ItemType Directory -Path $newPlugin, $newExe, $out | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'Release/d3d11_plugin_bench.exe') -Destination $newExe
foreach ($name in $files) {
    Copy-Item -LiteralPath (Join-Path $build "plugins/Release/$name") -Destination $newPlugin
}
$newHash = (Get-FileHash -LiteralPath (Join-Path $newPlugin 'gstproresd3d11.dll') `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($newHash -eq $oldHash) { throw '新旧DX11 DLLのSHA256が同じ' }
$records = @()
for ($pair = 0; $pair -lt 4; ++$pair) {
    $order = if ($pair % 2 -eq 0) { @('old','new') } else { @('new','old') }
    foreach ($variant in $order) {
        $tag = "p$pair-$variant"
        $directory = if ($variant -eq 'old') { $oldBuild } else { $newBuild }
        $trial = Join-Path $out $tag
        & python (Join-Path $root 'scripts/benchmark-d3d11-plugin.py') @sources `
            --modes dx11-direct --repeats 1 --loops 3 --warmup 30 `
            --frames-per-loop 180 --seeks 0 --startups 0 `
            --build-dir $directory --out $trial *> (Join-Path $out "$tag.log")
        if ($LASTEXITCODE) { throw "HQ direct測定失敗: $tag" }
        $rows = @(Get-Content (Join-Path $trial 'medians.json') -Raw | ConvertFrom-Json)
        if ($rows.Count -ne 2) { throw "HQ direct結果数が不正: $tag" }
        foreach ($row in $rows) {
            if ($row.mode -ne 'dx11-direct' -or $row.steady_fps -le 0 -or $row.gpu_completion_wait) {
                throw "測定境界が不正: $tag"
            }
            $records += [pscustomobject]@{
                pair=$pair; variant=$variant; input=$row.input; steady_fps=$row.steady_fps
            }
        }
        Write-Output "$tag HQ 1080/4K完了"
    }
}
$summary = @()
foreach ($name in @('synthetic-1080p60-hq.mov','synthetic-2160p60-hq.mov')) {
    $medians = @{}
    foreach ($variant in @('old','new')) {
        $values = @($records | Where-Object { $_.variant -eq $variant -and $_.input -eq $name } |
            ForEach-Object steady_fps | Sort-Object)
        if ($values.Count -ne 4) { throw "交互測定数が不正: $name/$variant" }
        $medians[$variant] = ($values[1] + $values[2]) / 2
    }
    $summary += [pscustomobject]@{
        input=$name; old_median_fps=$medians.old; new_median_fps=$medians.new
        change_percent=100 * ($medians.new / $medians.old - 1)
        passed=($medians.new / $medians.old -ge 0.97)
    }
}
[pscustomobject]@{
    pairs=4; old_plugin_sha256=$oldHash; new_plugin_sha256=$newHash
    trials=$records; results=$summary
} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
$summary | Format-Table | Out-String | Write-Output
if (@($summary | Where-Object { !$_.passed }).Count) { throw 'HQ directの−3%ゲート未達' }
