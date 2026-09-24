# M1採用版とM2作業版のHQ directを同一素材・交互4組で比較する。
[CmdletBinding()]
param([string]$OutDir = 'results/prores-m2-2026-09-24/hq-direct-ab')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/vs18'
$oldBuild = Join-Path $build 'profile-422-direct-ab-new'
$newBuild = Join-Path $build 'prores-m2-direct-ab-new'
$newSource = Join-Path $build 'plugins/Release'
$out = Join-Path $root $OutDir
$names = @('gstproresd3d11.dll','prores_vld.cso','prores_idct_unorm.cso','prores_rgb.cso')
$sources = @((Join-Path $root 'media/synthetic-1080p60-hq.mov'),
             (Join-Path $root 'media/synthetic-2160p60-hq.mov'))
$m1 = Get-Content -LiteralPath (Join-Path $root 'results/profile-422-direct-ab-2026-09-24/summary.json') -Raw |
    ConvertFrom-Json
if ((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath $newBuild)) {
    throw '既存のM2結果・配置は上書きしません'
}
foreach ($path in $sources + @($names | ForEach-Object { Join-Path $oldBuild "plugins/Release/$_" }) +
    @($names | ForEach-Object { Join-Path $newSource $_ }) +
    @((Join-Path $oldBuild 'Release/d3d11_plugin_bench.exe'),
      (Join-Path $build 'Release/d3d11_plugin_bench.exe'))) {
    if (!(Test-Path -LiteralPath $path)) { throw "比較入力がありません: $path" }
}
$oldHash = (Get-FileHash -LiteralPath (Join-Path $oldBuild 'plugins/Release/gstproresd3d11.dll') `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($oldHash -ne $m1.new_plugin_sha256 -or !$m1.results -or
    @($m1.results | Where-Object { !$_.passed }).Count) {
    throw 'M1採用版の由来を確認できません'
}
$newPlugin = Join-Path $newBuild 'plugins/Release'
$newExe = Join-Path $newBuild 'Release'
New-Item -ItemType Directory -Force -Path $newPlugin, $newExe, $out | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'Release/d3d11_plugin_bench.exe') -Destination $newExe
foreach ($name in $names) { Copy-Item -LiteralPath (Join-Path $newSource $name) -Destination $newPlugin }
$newHash = (Get-FileHash -LiteralPath (Join-Path $newPlugin 'gstproresd3d11.dll') `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($newHash -eq $oldHash) { throw '新旧DX11 DLLのSHA256が同じです' }
$records = @()
for ($pair = 0; $pair -lt 4; ++$pair) {
    $order = if ($pair % 2 -eq 0) { @('old','new') } else { @('new','old') }
    foreach ($variant in $order) {
        $tag = "p$pair-$variant"
        $trial = Join-Path $out $tag
        $directory = if ($variant -eq 'old') { $oldBuild } else { $newBuild }
        & python (Join-Path $root 'scripts/benchmark-d3d11-plugin.py') @sources `
            --modes dx11-direct --repeats 1 --loops 3 --warmup 30 `
            --frames-per-loop 180 --seeks 0 --startups 0 `
            --build-dir $directory --out $trial *> (Join-Path $out "$tag.log")
        if ($LASTEXITCODE) { throw "HQ direct測定失敗: $tag" }
        $rows = @(Get-Content -LiteralPath (Join-Path $trial 'medians.json') -Raw | ConvertFrom-Json)
        if ($rows.Count -ne 2) { throw "HQ 1080p/4K結果数が不正: $tag" }
        foreach ($row in $rows) {
            if ($row.mode -ne 'dx11-direct' -or $row.steady_fps -le 0 -or $row.gpu_completion_wait) {
                throw "HQ direct測定条件が不正: $tag"
            }
            $records += [pscustomobject]@{
                pair=$pair; variant=$variant; input=$row.input; steady_fps=$row.steady_fps
                plugin_sha256=$(if ($variant -eq 'old') { $oldHash } else { $newHash })
            }
        }
        Write-Host "$tag`: 1080p/4K完了"
    }
}
$summary = @()
foreach ($name in @('synthetic-1080p60-hq.mov','synthetic-2160p60-hq.mov')) {
    $medians = @{}
    foreach ($variant in @('old','new')) {
        $values = @($records | Where-Object { $_.variant -eq $variant -and $_.input -eq $name } |
            ForEach-Object { [double]$_.steady_fps } | Sort-Object)
        if ($values.Count -ne 4) { throw "測定回数が不正: $name/$variant" }
        $medians[$variant] = ($values[1] + $values[2]) / 2
    }
    $summary += [pscustomobject]@{
        input=$name; old_median_fps=$medians.old; new_median_fps=$medians.new
        change_percent=100 * ($medians.new / $medians.old - 1)
        passed=($medians.new / $medians.old -ge 0.97)
    }
}
[pscustomobject]@{
    pairs=4; old_source='M1採用版（profile-422-direct-ab-new）'
    old_plugin_sha256=$oldHash; new_plugin_sha256=$newHash
    trials=$records; results=$summary
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
if (@($summary | Where-Object { !$_.passed }).Count) {
    throw 'M2 HQ direct性能ゲート未達。summary.jsonを参照'
}
$summary | Format-Table | Out-String | Write-Host
