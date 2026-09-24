# GStreamer D3D11Memoryの候補formatを、実際のpool/UAV作成で判定する。
param(
    [string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64',
    [string]$Probe = 'build/vs18/Release/d3d11_memory_probe.exe',
    [string]$Output = 'results/feature-matrix-2026-09-24/d3d11-formats.json'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Probe)) { throw "probeがありません: $Probe" }
$savedPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $GStreamerRoot 'bin');$savedPath"
    $version = & (Join-Path $GStreamerRoot 'bin/gst-inspect-1.0.exe') --version | Select-Object -First 2
    $formats = @(
        'I422_10LE','Y444_10LE','I422_12LE','Y444_12LE',
        'A422_10LE','A444_10LE','A422_12LE','A444_12LE',
        'AYUV64','Y412_LE','Y416_LE','RGBA64_LE','BGRA64_LE',
        'AYUV','VUYA','RGBA','BGRA','RGB10A2_LE','Y410'
    )
    $rows = foreach ($format in $formats) {
        $lines = @(& $Probe $format 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
        $json = $lines | Where-Object { $_ -match '^\{' } | Select-Object -First 1
        $data = if ($json) { $json | ConvertFrom-Json } else { $null }
        $row = [pscustomobject]@{
            format=$format; exitCode=$code; supported=($data -ne $null)
            memories=if ($data) { $data.memories } else { $null }
            dxgiFormat=if ($data) { $data.planes[0].dxgi_format } else { $null }
            allPlanesUav=if ($data) { $data.all_planes_uav } else { $false }
            usable=($code -eq 0 -and $data -ne $null -and $data.all_planes_uav)
            error=if ($code -ne 0) { ($lines | Select-Object -Last 1) } else { $null }
        }
        Write-Host "$format`: usable=$($row.usable) memories=$($row.memories)"
        $row
    }
    [pscustomobject]@{ gstreamerVersion=$version; adapterIndex=0; width=1920; height=1080; cases=@($rows) } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
} finally {
    $env:PATH = $savedPath
}
