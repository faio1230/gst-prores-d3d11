# DX11版だけを内部検証用ディレクトリに配置し、固定SDKから独立してロードできるか確認する。
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/vs18',
    [string]$OutDir = 'build/vs18/stage-d3d11-internal',
    [string]$SummaryOut = 'results/stage-d3d11-internal-2026-09-24.json',
    [string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root $BuildDir
$out = [IO.Path]::GetFullPath((Join-Path $root $OutDir))
$allowed = [IO.Path]::GetFullPath($build) + [IO.Path]::DirectorySeparatorChar
if (!$out.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $out)) {
    throw '出力先は未作成のbuildディレクトリ内を指定してください（既存物は上書きしません）'
}
$artifactDir = Join-Path $build 'plugins/Release'
$gstBin = Join-Path $GStreamerRoot 'bin'
$inspect = Join-Path $gstBin 'gst-inspect-1.0.exe'
$launch = Join-Path $gstBin 'gst-launch-1.0.exe'
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$fixture1080 = Join-Path $root 'media/synthetic-1080p60-hq.mov'
$fixture4k = Join-Path $root 'media/synthetic-2160p60-hq.mov'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$files = @(
    'gstproresd3d11.dll', 'prores_vld.cso', 'prores_idct_unorm.cso', 'prores_rgb.cso',
    'prores_vld.hlsl', 'prores_idct.hlsl', 'prores_rgb.hlsl'
)
$docs = @('GStreamerプラグイン.md', '採用判断サマリー.md', 'DX11実装検証.md')
foreach ($required in @($inspect, $launch, $ffmpeg, $fixture1080, $fixture4k, $vswhere,
    (Join-Path $root 'packaging/README-ja.md')) +
    @($files | ForEach-Object { Join-Path $artifactDir $_ }) +
    @($docs | ForEach-Object { Join-Path (Join-Path $root 'docs') $_ })) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
if (!$vs) { throw 'MSVCの依存DLL検査器がありません' }
$dumpbin = Get-ChildItem (Join-Path $vs.installationPath 'VC/Tools/MSVC') -Recurse -Filter dumpbin.exe |
    Where-Object FullName -Match 'Hostx64\\x64' | Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (!$dumpbin) { throw 'x64 dumpbin.exeがありません' }
$originalDll = Join-Path $artifactDir 'gstproresd3d11.dll'
$headers = (& $dumpbin /headers $originalDll | Out-String)
if ($LASTEXITCODE -ne 0 -or $headers -notmatch '8664 machine \(x64\)') {
    throw 'プラグインDLLはx64 PEではありません'
}
$importsText = (& $dumpbin /dependents $originalDll | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'プラグインDLLの直接依存を読めません' }
$imports = @([regex]::Matches($importsText, '(?im)^\s+([A-Za-z0-9_.-]+\.dll)\s*$') |
    ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() })
if (!$imports.Count -or $imports -notcontains 'gstd3d11-1.0-0.dll' -or
    $imports -notcontains 'gstreamer-1.0-0.dll' -or
    @($imports | Where-Object { $_ -match '^(avcodec|avutil|swscale|swresample|vulkan|d3dcompiler|gstproresvk).*\.dll$' }).Count) {
    throw 'DX11プラグインの直接依存に不足または禁止DLLがあります'
}

New-Item -ItemType Directory -Path $out | Out-Null
foreach ($file in $files) {
    Copy-Item -LiteralPath (Join-Path $artifactDir $file) -Destination (Join-Path $out $file)
}
Copy-Item -LiteralPath (Join-Path $root 'packaging/README-ja.md') -Destination (Join-Path $out 'README-ja.md')
$docOut = Join-Path $out 'docs'
New-Item -ItemType Directory -Path $docOut | Out-Null
foreach ($doc in $docs) {
    Copy-Item -LiteralPath (Join-Path (Join-Path $root 'docs') $doc) -Destination (Join-Path $docOut $doc)
}
if ((Get-ChildItem -LiteralPath $out -File | Where-Object Name -Match '(?i)vulkan|gstproresvk|ffmpeg')) {
    throw 'DX11ステージへ比較用ファイルが混入しました'
}
$savedPath = $env:PATH
$savedPluginPath = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$savedShaderDirectory = $env:PRORES_DX11_SHADER_DIR
$log = Join-Path $build 'stage-d3d11-internal-test.log'
try {
    $env:PATH = "$gstBin;$savedPath"
    $env:GST_PLUGIN_PATH = $out
    $env:GST_REGISTRY = Join-Path $build ("stage-d3d11-registry-" + [guid]::NewGuid().ToString('N') + '.bin')
    Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    Push-Location -LiteralPath $out
    foreach ($element in @('proresd3d11dec', 'proresd3d11rgb')) {
        $details = (& $inspect $element 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0 -or $details -notmatch [regex]::Escape((Join-Path $out 'gstproresd3d11.dll'))) {
            throw "ステージの要素をロードできません: $element"
        }
    }
    $pipelineResults = @()
    foreach ($fixture in @($fixture1080, $fixture4k)) {
        $inputPath = $fixture -replace '\\', '/'
        & $launch -q -e filesrc "location=$inputPath" ! qtdemux ! proresd3d11dec ! `
            'video/x-raw(memory:D3D11Memory),format=I422_10LE' ! fakesink sync=false `
            *> $log
        if ($LASTEXITCODE -ne 0) { throw "ステージのDX11復号に失敗: $fixture ($log)" }
        $pipelineResults += [ordered]@{ input = (Split-Path $fixture -Leaf); output = 'I422_10LE D3D11Memory'; passed = $true }
    }
    $tagged = Join-Path $build 'stage-d3d11-bt709-fixture.mov'
    & $ffmpeg -hide_banner -loglevel error -i $fixture1080 -map 0:v:0 -c:v copy `
        -color_primaries bt709 -color_trc bt709 -colorspace bt709 -movflags +write_colr -y $tagged
    if ($LASTEXITCODE -ne 0) { throw '固定FFmpeg SDKによる色付き検査素材の作成に失敗' }
    $rgbInput = $tagged -replace '\\', '/'
    & $launch -q -e filesrc "location=$rgbInput" ! qtdemux ! proresd3d11dec ! `
        proresd3d11rgb ! 'video/x-raw(memory:D3D11Memory),format=RGB10A2_LE' ! `
        fakesink sync=false *> $log
    if ($LASTEXITCODE -ne 0) { throw "ステージのDX11 RGB変換に失敗: $log" }
    $pipelineResults += [ordered]@{ input = 'stage-d3d11-bt709-fixture.mov'; output = 'RGB10A2_LE D3D11Memory'; passed = $true }
    $version = (& $inspect --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'GStreamer版を取得できません' }
    $commit = (& git -C $root rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Git revisionを取得できません' }
    $workingTreeDirty = @(& git -C $root status --porcelain).Count -gt 0
    if ($LASTEXITCODE -ne 0) { throw 'Git作業ツリー状態を取得できません' }
    $manifestNames = @($files + 'README-ja.md') + @($docs | ForEach-Object { "docs/$_" })
    $manifestFiles = @($manifestNames | ForEach-Object {
        $path = Join-Path $out $_
        [ordered]@{
            name = $_
            bytes = (Get-Item -LiteralPath $path).Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    $manifest = [ordered]@{
        status = '内部検証用。公開配布の採用判定ではない'
        repository_head_at_staging = $commit
        working_tree_dirty_at_staging = $workingTreeDirty
        isolated_working_directory = $out
        gstreamer_sdk = $version
        architecture = 'x64'
        direct_dll_dependencies = $imports
        files = $manifestFiles
        isolated_pipeline_checks = $pipelineResults
        outstanding_gates = @('公開ライセンスと対応ソースの提供方法', '実表示の安定性', '他GPUと実際のdevice lost')
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 5
    $manifestJson | Set-Content -LiteralPath (Join-Path $out 'manifest.json') -Encoding utf8
    $summaryPath = Join-Path $root $SummaryOut
    New-Item -ItemType Directory -Force (Split-Path $summaryPath -Parent) | Out-Null
    $manifestJson | Set-Content -LiteralPath $summaryPath -Encoding utf8
    Write-Host "DX11内部検証ステージ成功: $out"
} finally {
    if ((Get-Location).Path -eq $out) { Pop-Location }
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPluginPath
    $env:GST_REGISTRY = $savedRegistry
    if ($null -eq $savedShaderDirectory) {
        Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    } else {
        $env:PRORES_DX11_SHADER_DIR = $savedShaderDirectory
    }
}
