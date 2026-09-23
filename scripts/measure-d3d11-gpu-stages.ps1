# 診断専用GPU timestampでVLDと逆DCTのshader区間を別々に測る。
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/vs18',
    [string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64',
    [string]$OutFile = 'results/gpu-stage-timing-2026-09-24.json',
    [string]$GpuLabel = '未記録',
    [int]$Repeats = 3,
    [int]$WarmupFrames = 5,
    [switch]$IncludeReal
)
$ErrorActionPreference = 'Stop'
if ($Repeats -lt 1 -or $WarmupFrames -lt 0) { throw '試行数またはウォームアップ枚数が不正です' }
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root $BuildDir
$gstBin = Join-Path $GStreamerRoot 'bin'
$launcher = Join-Path $gstBin 'gst-launch-1.0.exe'
$pluginDirectory = Join-Path $build 'plugins/Release'
$cases = @(
    @{ Name = 'synthetic-1080p60'; Input = 'media/synthetic-1080p60-hq.mov'; Frames = 180 },
    @{ Name = 'synthetic-2160p60'; Input = 'media/synthetic-2160p60-hq.mov'; Frames = 180 }
)
if ($IncludeReal) {
    $cases += @{ Name = 'dji-real-4k60'; Input = 'media/reference-dji-nature-4k60-rec709-hq.mov'; Frames = 480 }
}
foreach ($required in @($launcher, (Join-Path $pluginDirectory 'gstproresd3d11.dll'),
    (Join-Path $pluginDirectory 'prores_vld.cso'), (Join-Path $pluginDirectory 'prores_idct_unorm.cso'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
foreach ($case in $cases) {
    if ($WarmupFrames -ge $case.Frames) { throw 'ウォームアップ枚数が素材の全フレーム以上です' }
    if (!(Test-Path -LiteralPath (Join-Path $root $case.Input))) {
        throw "測定素材がありません: $($case.Input)"
    }
}
$logs = Join-Path $build 'gpu-stage-timing-logs'
$output = Join-Path $root $OutFile
New-Item -ItemType Directory -Force $logs, (Split-Path $output -Parent) | Out-Null
$savedPath = $env:PATH
$savedPluginPath = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$savedTiming = $env:PRORES_DX11_GPU_TIMING
$savedDebug = $env:GST_DEBUG
$savedNoColor = $env:GST_DEBUG_NO_COLOR
$savedShaderDirectory = $env:PRORES_DX11_SHADER_DIR
function Get-Distribution([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { throw '測定値がありません' }
    return [ordered]@{
        median_ms = $sorted[[int][math]::Ceiling(0.50 * ($sorted.Count - 1))]
        p95_ms = $sorted[[int][math]::Ceiling(0.95 * ($sorted.Count - 1))]
        p99_ms = $sorted[[int][math]::Ceiling(0.99 * ($sorted.Count - 1))]
        max_ms = $sorted[-1]
    }
}
try {
    $env:PATH = "$gstBin;$savedPath"
    $env:GST_PLUGIN_PATH = $pluginDirectory
    $env:GST_REGISTRY = Join-Path $build 'gpu-stage-timing-registry.bin'
    $env:PRORES_DX11_GPU_TIMING = '1'
    $env:GST_DEBUG = 'proresd3d11dec:4'
    $env:GST_DEBUG_NO_COLOR = '1'
    Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    $summaries = @()
    foreach ($case in $cases) {
        $vld = [Collections.Generic.List[double]]::new()
        $idct = [Collections.Generic.List[double]]::new()
        $copy = [Collections.Generic.List[double]]::new()
        $vldToCopy = [Collections.Generic.List[double]]::new()
        $all = [Collections.Generic.List[double]]::new()
        for ($run = 1; $run -le $Repeats; ++$run) {
            $log = Join-Path $logs "$($case.Name)-run$run.log"
            $inputPath = (Join-Path $root $case.Input) -replace '\\', '/'
            & $launcher -q -e filesrc "location=$inputPath" ! qtdemux ! proresd3d11dec ! `
                fakesink sync=false *> $log
            if ($LASTEXITCODE) { throw "GPU段階計測が失敗: $log" }
            $content = Get-Content -LiteralPath $log -Raw
            if ($content.Contains('GPU_STAGE_DISJOINT')) { throw "GPU時刻の不連続を検出: $log" }
            $rows = [regex]::Matches($content,
                'GPU_STAGE pts_ns=(\d+) vld_ms=([\d.]+) idct_ms=([\d.]+) copy_ms=([\d.]+) vld_to_copy_ms=([\d.]+)')
            if ($rows.Count -ne $case.Frames) {
                throw "GPU時刻の件数が不一致: $log 期待=$($case.Frames) 実際=$($rows.Count)"
            }
            $pts = [Collections.Generic.HashSet[string]]::new()
            for ($index = 0; $index -lt $rows.Count; ++$index) {
                $row = $rows[$index]
                if (!$pts.Add($row.Groups[1].Value)) { throw "PTSが重複: $log" }
                if ($index -lt $WarmupFrames) { continue }
                $v = [double]::Parse($row.Groups[2].Value,
                    [Globalization.CultureInfo]::InvariantCulture)
                $i = [double]::Parse($row.Groups[3].Value,
                    [Globalization.CultureInfo]::InvariantCulture)
                $c = [double]::Parse($row.Groups[4].Value,
                    [Globalization.CultureInfo]::InvariantCulture)
                $vc = [double]::Parse($row.Groups[5].Value,
                    [Globalization.CultureInfo]::InvariantCulture)
                if ($v -le 0 -or $i -le 0 -or $c -lt 0 -or $vc -lt $c) {
                    throw "GPU区間の時刻が不正: $log"
                }
                $vld.Add($v)
                $idct.Add($i)
                $copy.Add($c)
                $vldToCopy.Add($vc)
                $all.Add($v + $i)
            }
        }
        $summaries += [ordered]@{
            name = $case.Name
            input = $case.Input
            sha256 = (Get-FileHash -LiteralPath (Join-Path $root $case.Input) -Algorithm SHA256).Hash.ToLowerInvariant()
            frames_per_run = $case.Frames
            repeats = $Repeats
            warmup_frames_per_run = $WarmupFrames
            measured_frames = $vld.Count
            vld = Get-Distribution $vld.ToArray()
            idct = Get-Distribution $idct.ToArray()
            copy = Get-Distribution $copy.ToArray()
            vld_to_copy = Get-Distribution $vldToCopy.ToArray()
            shader_total = Get-Distribution $all.ToArray()
        }
        Write-Host "$($case.Name): VLD中央値=$([math]::Round($summaries[-1].vld.median_ms, 3))ms、IDCT中央値=$([math]::Round($summaries[-1].idct.median_ms, 3))ms"
    }
    [ordered]@{
        passed = $true
        timing_mode = 'PRORES_DX11_GPU_TIMING=1; D3D11 timestamp/disjoint; VLD/IDCT dispatch and VLD error staging copy'
        gpu = $GpuLabel
        caveat = 'Diagnostic queries wait for IDCT completion per frame; not production throughput or full decode latency.'
        cases = $summaries
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $output -Encoding utf8
    Write-Host "保存: $output"
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPluginPath
    $env:GST_REGISTRY = $savedRegistry
    $env:PRORES_DX11_GPU_TIMING = $savedTiming
    $env:GST_DEBUG = $savedDebug
    $env:GST_DEBUG_NO_COLOR = $savedNoColor
    $env:PRORES_DX11_SHADER_DIR = $savedShaderDirectory
}
