# 診断専用swap chainのPresent(0/1)を、同じGStreamer入力で交互反復する。
[CmdletBinding()]
param(
    [string]$Source = 'media/reference-dji-nature-4k60-rec709-hq.mov',
    [int]$Repeats = 2,
    [int]$MaxFrames = 480,
    [switch]$Topmost,
    [string]$OutDir = 'results/present-control-dji-4k60',
    [string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64'
)
$ErrorActionPreference = 'Stop'
if ($Repeats -lt 1 -or $MaxFrames -lt 1) { throw 'RepeatsとMaxFramesは正の整数が必要です' }
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build/vs18'
$gstBin = Join-Path $GStreamerRoot 'bin'
$exe = Join-Path $build 'Release/d3d11_present_control.exe'
$presentMon = Join-Path $build 'diagnostic-tools/PresentMon-2.6.0-x64.exe'
$pluginDirectory = Join-Path $build 'plugins/Release'
$sourcePath = if ($Source -eq 'testsrc') { 'testsrc' } else { Join-Path $root $Source }
$out = Join-Path $root $OutDir
foreach ($required in @($exe, $presentMon, (Join-Path $pluginDirectory 'gstproresd3d11.dll'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
if ($Source -ne 'testsrc' -and !(Test-Path -LiteralPath $sourcePath)) {
    throw "入力がありません: $sourcePath"
}
New-Item -ItemType Directory -Force $out | Out-Null
$savedPath = $env:PATH
$savedPluginPath = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$savedShaderDirectory = $env:PRORES_DX11_SHADER_DIR
$sessionPrefix = 'PRPC' + (Get-Date -Format 'MMddHHmmss')
try {
    $env:PATH = "$gstBin;$savedPath"
    $env:GST_PLUGIN_PATH = $pluginDirectory
    $env:GST_REGISTRY = Join-Path $build 'plugin-d3d11-present-control-registry.bin'
    Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    for ($repeat = 0; $repeat -lt $Repeats; ++$repeat) {
        $intervals = if ($repeat % 2 -eq 0) { @(0, 1) } else { @(1, 0) }
        foreach ($interval in $intervals) {
            $tag = "r$repeat-s$interval"
            $timeline = Join-Path $out "$tag-present.csv"
            $capture = Join-Path $out "$tag-presentmon.csv"
            $log = Join-Path $out "$tag.log"
            $monitorLog = Join-Path $out "$tag-presentmon.log"
            $monitorError = Join-Path $out "$tag-presentmon-error.log"
            $session = "$sessionPrefix${repeat}${interval}"
            $monitor = Start-Process -FilePath $presentMon -ArgumentList @(
                '--process_name', 'd3d11_present_control.exe', '--output_file', $capture,
                '--qpc_time_ms', '--write_display_metadata', '--timed', '12',
                '--terminate_after_timed', '--no_console_stats', '--session_name', $session
            ) -WindowStyle Hidden -PassThru -RedirectStandardOutput $monitorLog `
                -RedirectStandardError $monitorError
            $completed = $false
            try {
                Start-Sleep -Milliseconds 1200
                $arguments = @($sourcePath, $interval, $timeline, $MaxFrames)
                if ($Topmost) { $arguments += 'topmost' }
                $output = & $exe @arguments 2>&1
                $code = $LASTEXITCODE
                $output | Set-Content -LiteralPath $log -Encoding utf8
                if ($code -ne 0) { throw "Present試行失敗: $tag、$log" }
                if (!$monitor.WaitForExit(20000)) { throw "PresentMonの終了待ちが時間切れ: $tag" }
                if ($monitor.ExitCode -ne 0 -or !(Test-Path -LiteralPath $capture)) {
                    throw "PresentMonの取得失敗: $tag (exit=$($monitor.ExitCode)、$monitorError)"
                }
                $completed = $true
                Write-Host "$tag`: $($output -join ' ')"
            } finally {
                if (!$completed) {
                    if (!$monitor.HasExited) {
                        Stop-Process -Id $monitor.Id -ErrorAction SilentlyContinue
                    }
                    & logman stop $session -ets *> $null
                }
                $monitor.Dispose()
            }
            Start-Sleep -Milliseconds 800
        }
    }
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPluginPath
    $env:GST_REGISTRY = $savedRegistry
    if ($null -eq $savedShaderDirectory) {
        Remove-Item Env:PRORES_DX11_SHADER_DIR -ErrorAction SilentlyContinue
    } else {
        $env:PRORES_DX11_SHADER_DIR = $savedShaderDirectory
    }
}
