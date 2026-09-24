# 標準GStreamerの2経路でAYUV64→RGBA64を全画素検査する。
[CmdletBinding()]
param([string]$OutDir = 'results/alpha-standard-converters-2026-09-24')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$gst = 'C:/Program Files/gstreamer/1.0/msvc_x86_64/bin'
$launch = Join-Path $gst 'gst-launch-1.0.exe'
$out = Join-Path $root $OutDir
if (Test-Path -LiteralPath $out) { throw "既存結果を上書きしません: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
$savedPath = $env:PATH
$savedPlugin = $env:GST_PLUGIN_PATH
$savedRegistry = $env:GST_REGISTRY
$records = @()
try {
    $env:PATH = "$gst;$savedPath"
    $env:GST_PLUGIN_PATH = Join-Path $root 'build/vs18/plugins/Release'
    $env:GST_REGISTRY = Join-Path $out 'gst-registry.bin'
    foreach ($name in @('apch-422-alpha8-varying-bt709', 'ap4h-alpha16-varying')) {
        $input = Join-Path $root "build/alpha-standard/$name.mov"
        if (!(Test-Path -LiteralPath $input)) { throw "素材がありません: $input" }
        $source = $input.Replace([char]92, [char]47)
        $raw = @{}
        foreach ($method in @('gpu', 'cpu', 'ayuv')) {
            $raw[$method] = Join-Path $out "$name-$method.raw"
            $destination = $raw[$method].Replace([char]92, [char]47)
            $chain = switch ($method) {
                'gpu' { @('d3d11convert', '!',
                    'video/x-raw(memory:D3D11Memory),format=RGBA64_LE', '!',
                    'd3d11download', '!', 'video/x-raw,format=RGBA64_LE') }
                'cpu' { @('d3d11download', '!', 'video/x-raw,format=AYUV64', '!',
                    'videoconvert', '!', 'video/x-raw,format=RGBA64_LE') }
                'ayuv' { @('d3d11download', '!', 'video/x-raw,format=AYUV64') }
            }
            $log = Join-Path $out "$name-$method.log"
            & $launch -e -v filesrc "location=$source" ! qtdemux ! proresd3d11dec ! `
                @chain ! filesink "location=$destination" *> $log
            if ($LASTEXITCODE) { throw "標準変換失敗: $name/$method ($log)" }
            $content = Get-Content -LiteralPath $log -Raw
            if ($content -notmatch 'format=\(string\)AYUV64' -or
                $content -match 'prores-depth|prores-chroma-shift' -or
                $content -notmatch 'colorimetry=\(string\)bt709' -or
                $content -notmatch 'Got EOS') {
                throw "AYUV64標準caps/BT.709/EOS不一致: $name/$method"
            }
        }
        $json = Join-Path $out "$name-comparison.json"
        & python (Join-Path $root 'scripts/check-alpha-standard-converters.py') `
            $input $raw.gpu $raw.cpu --ayuv-raw $raw.ayuv --out $json `
            *> (Join-Path $out "$name-comparison.log")
        if ($LASTEXITCODE) { throw "標準変換の全画素比較失敗: $name" }
        $comparison = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
        if (!$comparison.passed -or @($comparison.results | Where-Object {
            $_.alpha_min -eq $_.alpha_max }).Count) {
            throw "可変alphaが保持されていない: $name"
        }
        $records += [pscustomobject]@{
            input="build/alpha-standard/$name.mov"
            sha256=(Get-FileHash -LiteralPath $input -Algorithm SHA256).Hash.ToLowerInvariant()
            comparison=$comparison
        }
        Write-Output "$name standard GPU/CPU conversion passed"
    }
} finally {
    $env:PATH = $savedPath
    $env:GST_PLUGIN_PATH = $savedPlugin
    $env:GST_REGISTRY = $savedRegistry
}
$records | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
