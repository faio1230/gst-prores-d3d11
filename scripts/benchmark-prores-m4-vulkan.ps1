# M4素材を同じRTX 3070の固定SDK Vulkanで測る。DX11 directとは境界が違う。
[CmdletBinding()]
param([string]$OutDir = 'results/m4-vulkan-performance-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$out = Join-Path $root $OutDir
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$records = @()
foreach ($size in @('1080p30','4k60')) {
    $frames = if ($size -eq '1080p30') { 30 } else { 60 }
    foreach ($format in @('apch','ap4h-alpha16')) {
        $pixels = if ($format -eq 'apch') { 'yuv422p10le' } else { 'yuva444p12le' }
        foreach ($order in @('tff','bff')) {
            $name = "$format-$order-$size"
            $input = Join-Path $root "build/m4-performance/$name.mov"
            $trials = @()
            foreach ($method in @('wrapped','download')) {
                for ($repeat = 0; $repeat -lt 3; ++$repeat) {
                    $arguments = @('-hide_banner','-nostats','-loglevel','verbose','-benchmark','-xerror',
                        '-init_hw_device','vulkan=vk:0','-filter_hw_device','vk',
                        '-hwaccel','vulkan','-hwaccel_output_format','vulkan',
                        '-i',$input,'-frames:v',[string]$frames)
                    if ($method -eq 'wrapped') {
                        $arguments += @('-noautoscale','-c:v','wrapped_avframe')
                    } else {
                        $arguments += @('-vf',"hwdownload,format=$pixels")
                    }
                    $arguments += @('-f','null','NUL')
                    for ($attempt = 0; $attempt -lt 2; ++$attempt) {
                        $log = Join-Path $out "$name-$method-r$repeat-a$attempt.log"
                        $start = [Diagnostics.ProcessStartInfo]::new($ffmpeg)
                        $start.UseShellExecute = $false
                        $start.RedirectStandardOutput = $true
                        $start.RedirectStandardError = $true
                        foreach ($argument in $arguments) { [void]$start.ArgumentList.Add([string]$argument) }
                        $process = [Diagnostics.Process]::Start($start)
                        $stdout = $process.StandardOutput.ReadToEndAsync()
                        $stderr = $process.StandardError.ReadToEndAsync()
                        $finished = $process.WaitForExit(20000)
                        if (!$finished) { $process.Kill($true); $process.WaitForExit() }
                        $content = $stderr.GetAwaiter().GetResult() + $stdout.GetAwaiter().GetResult()
                        $content | Set-Content -LiteralPath $log -Encoding utf8
                        $code = if ($finished) { $process.ExitCode } else { -1 }
                        $process.Dispose()
                        $countMatches = [regex]::Matches($content, '(\d+) frames encoded')
                        $count = if ($countMatches.Count) { [int]$countMatches[-1].Groups[1].Value } else { 0 }
                        $timeMatches = [regex]::Matches($content,
                            'bench: utime=[0-9.]+s stime=[0-9.]+s rtime=([0-9.]+)s')
                        $seconds = if ($timeMatches.Count) { [double]::Parse($timeMatches[-1].Groups[1].Value,
                            [Globalization.CultureInfo]::InvariantCulture) } else { 0 }
                        if ($code -eq 0 -and $count -eq $frames -and $seconds -gt 0 -and
                            $content -match 'pixfmt:vulkan' -and
                            $content -match 'Device 0 selected: NVIDIA GeForce RTX 3070' -and
                            $content -match '0 decode errors') { break }
                        if ($attempt -eq 1) {
                            throw "Vulkan GPU速度測定失敗: $name/$method/$repeat ($log)"
                        }
                    }
                    $trials += [pscustomobject]@{method=$method; repeat=$repeat; attempts=$attempt+1; frames=$count;
                        seconds=$seconds; fps=$count/$seconds}
                }
            }
            $records += [pscustomobject]@{
                input="build/m4-performance/$name.mov"
                sha256=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant()
                wrapped_median_fps=@($trials | Where-Object method -EQ 'wrapped' |
                    ForEach-Object fps | Sort-Object)[1]
                download_median_fps=@($trials | Where-Object method -EQ 'download' |
                    ForEach-Object fps | Sort-Object)[1]
                trials=$trials
                note='Vulkanは各素材を1周、起動込み。DX11 directと測定境界が異なるため比率で優劣判定しない'
            }
            $records | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
            Write-Output "$name Vulkan wrapped/download完了"
        }
    }
}
