# M1作業ツリーと直前の採用版を、HQ同一素材でdirect交互4組比較する。
param(
    [ValidateRange(1, 5)][int]$Pairs = 4,
    [string]$OutDir = 'results/profile-422-direct-ab-2026-09-24'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$build = Join-Path $root 'build/vs18'
$oldStage = Join-Path $build 'stage-vld-ring-restored-b08e1d9'
$newSource = Join-Path $build 'plugins/Release'
$bench = Join-Path $build 'Release/d3d11_plugin_bench.exe'
$oldBuild = Join-Path $build 'profile-422-direct-ab-old'
$newBuild = Join-Path $build 'profile-422-direct-ab-new'
$names = @('gstproresd3d11.dll','prores_vld.cso','prores_idct_unorm.cso','prores_rgb.cso')
$sources = @((Join-Path $root 'media/synthetic-1080p60-hq.mov'),
             (Join-Path $root 'media/synthetic-2160p60-hq.mov'))
if ((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath $oldBuild) -or
    (Test-Path -LiteralPath $newBuild)) { throw '既存の結果・配置は上書きしません' }
foreach ($path in @($bench) + $sources + @($names | ForEach-Object { Join-Path $oldStage $_ }) +
    @($names | ForEach-Object { Join-Path $newSource $_ })) {
    if (-not (Test-Path -LiteralPath $path)) { throw "入力がありません: $path" }
}
$oldManifest = Get-Content -LiteralPath (Join-Path $oldStage 'manifest.json') -Raw | ConvertFrom-Json
if ($oldManifest.repository_head_at_staging -ne 'b08e1d915c5a5758e1eb425a422e3a1974c1ab33' -or
    $oldManifest.working_tree_dirty_at_staging -or -not $oldManifest.source_rebuild.passed) {
    throw '旧版ステージの由来を確認できません'
}
& git diff --quiet b08e1d9 77c9672 -- src packaging/CMakeLists.txt
if ($LASTEXITCODE) { throw '旧版ステージからM1直前までの実装差分があります' }
$hashes = @{}
foreach ($variant in @('old','new')) {
    $buildDir = if ($variant -eq 'old') { $oldBuild } else { $newBuild }
    $sourceDir = if ($variant -eq 'old') { $oldStage } else { $newSource }
    $pluginDir = Join-Path $buildDir 'plugins/Release'
    $exeDir = Join-Path $buildDir 'Release'
    New-Item -ItemType Directory -Path $pluginDir, $exeDir -Force | Out-Null
    Copy-Item -LiteralPath $bench -Destination $exeDir
    foreach ($name in $names) {
        $source = Join-Path $sourceDir $name
        if ($variant -eq 'old') {
            $expected = $oldManifest.files | Where-Object name -EQ $name | Select-Object -First 1
            if (-not $expected -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $expected.sha256) {
                throw "旧版SHA256が不一致: $name"
            }
        }
        Copy-Item -LiteralPath $source -Destination $pluginDir
    }
    $hashes[$variant] = (Get-FileHash -LiteralPath (Join-Path $pluginDir 'gstproresd3d11.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ($hashes.old -eq $hashes.new) { throw '新旧DLLのSHA256が同じです' }
New-Item -ItemType Directory -Path $out | Out-Null
$python = (Get-Command python -ErrorAction Stop).Source
$records = @()
for ($pair = 0; $pair -lt $Pairs; ++$pair) {
    $order = if ($pair % 2 -eq 0) { @('old','new') } else { @('new','old') }
    foreach ($variant in $order) {
        $tag = "p$pair-$variant"
        $trial = Join-Path $out $tag
        $buildDir = if ($variant -eq 'old') { $oldBuild } else { $newBuild }
        & $python (Join-Path $root 'scripts/benchmark-d3d11-plugin.py') @sources `
            --modes dx11-direct --repeats 1 --loops 3 --warmup 30 --frames-per-loop 180 `
            --seeks 0 --startups 0 --build-dir $buildDir --out $trial `
            *> (Join-Path $out "$tag.log")
        if ($LASTEXITCODE) { throw "direct計測に失敗: $tag" }
        $rows = @(Get-Content -LiteralPath (Join-Path $trial 'medians.json') -Raw | ConvertFrom-Json)
        if ($rows.Count -ne 2) { throw "1080p/4K測定数が不一致: $tag" }
        foreach ($row in $rows) {
            if ($row.mode -ne 'dx11-direct' -or $row.steady_fps -le 0 -or $row.gpu_completion_wait) {
                throw "direct計測条件が不正: $tag"
            }
            $records += [pscustomobject]@{
                pair=$pair; variant=$variant; input=$row.input; steady_fps=$row.steady_fps
                plugin_sha256=$hashes[$variant]
            }
        }
        Write-Host "$tag`: 1080p/4K完了"
    }
}
$summary = @()
foreach ($inputName in @('synthetic-1080p60-hq.mov','synthetic-2160p60-hq.mov')) {
    $medians = @{}
    foreach ($variant in @('old','new')) {
        $values = @($records | Where-Object { $_.variant -eq $variant -and $_.input -eq $inputName } |
            ForEach-Object { [double]$_.steady_fps } | Sort-Object)
        if ($values.Count -ne $Pairs) { throw "計測回数が不一致: $inputName/$variant" }
        $mid = [int][math]::Floor($Pairs / 2)
        $medians[$variant] = if ($Pairs % 2) { $values[$mid] } else { ($values[$mid-1] + $values[$mid]) / 2 }
    }
    $summary += [pscustomobject]@{
        input=$inputName; old_median_fps=$medians.old; new_median_fps=$medians.new
        change_percent=100 * ($medians.new / $medians.old - 1)
        passed=($medians.new / $medians.old -ge 0.97)
    }
}
[pscustomobject]@{
    pairs=$Pairs; old_source_commit='b08e1d9（77c9672まで実装差分なし）'
    old_plugin_sha256=$hashes.old; new_plugin_sha256=$hashes.new
    trials=$records; results=$summary
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
if (@($summary | Where-Object { -not $_.passed }).Count) { throw 'HQ direct性能ゲート未達。summary.jsonを確認' }
$summary | Format-Table | Out-String | Write-Host
