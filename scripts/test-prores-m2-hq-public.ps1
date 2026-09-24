# M2後の既存公開HQ素材779枚をD3D11Memoryで全枚数・EOS回帰する。
[CmdletBinding()]
param([string]$OutDir = 'results/prores-m2-2026-09-24/hq-public')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root $OutDir
$bench = Join-Path $root 'build/vs18/Release/d3d11_plugin_bench.exe'
$launch = 'C:/Program Files/gstreamer/1.0/msvc_x86_64/bin/gst-launch-1.0.exe'
$cases = @(
    @{ Name='reference-proton-rec709-hq.mov'; Frames=50 },
    @{ Name='reference-dji-nature-4k24-hq.mov'; Frames=129 },
    @{ Name='reference-dji-nature-4k60-rec709-hq.mov'; Frames=480 },
    @{ Name='reference-slomo-4k-hq60-complete.mov'; Frames=120 }
)
New-Item -ItemType Directory -Force -Path $out | Out-Null
$savedPath = $env:PATH
$savedPlugin = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$records = @()
try {
    $env:PATH = "C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;$savedPath"
    $env:GST_PLUGIN_PATH = Join-Path $root 'build/vs18/plugins/Release'
    $env:GST_REGISTRY = Join-Path $root 'build/vs18/prores-m2-hq-public-registry.bin'
    foreach ($case in $cases) {
        $input = Join-Path $root "media/$($case.Name)"
        if (!(Test-Path -LiteralPath $input)) { throw "公開HQ素材がありません: $input" }
        $tag = [IO.Path]::GetFileNameWithoutExtension($input)
        $lines = @(& $bench $input dx11-direct (Join-Path $out "$tag.csv") `
            1 0 $case.Frames 0 0 0 1000000 2>&1 | ForEach-Object { "$_" })
        $lines | Set-Content -LiteralPath (Join-Path $out "$tag-direct.log") -Encoding utf8
        if ($LASTEXITCODE) { throw "公開HQ全枚数検査に失敗: $tag" }
        $direct = $lines | Select-Object -Last 1 | ConvertFrom-Json
        if (!$direct.passed -or $direct.frames -ne $case.Frames -or
            $direct.completed_loops -ne 1 -or $direct.gpu_completion_wait) {
            throw "公開HQ全枚数JSON不合格: $tag"
        }
        & $launch -q -e filesrc "location=$($input -replace '\\','/')" ! qtdemux ! `
            proresd3d11dec ! 'video/x-raw(memory:D3D11Memory),format=I422_10LE' ! `
            fakesink sync=false *> (Join-Path $out "$tag-eos.log")
        if ($LASTEXITCODE) { throw "公開HQ EOS検査に失敗: $tag" }
        $records += [pscustomobject]@{
            input="media/$($case.Name)"; sha256=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant()
            frames=$case.Frames; d3d11memory_frames=$direct.frames
            direct_d3d11memory=$true; eos=$true; passed=$true
        }
        $records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
        Write-Host "$tag`: $($case.Frames)枚、D3D11Memory/EOS成功"
    }
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPlugin
    $env:GST_REGISTRY = $savedRegistry
}
