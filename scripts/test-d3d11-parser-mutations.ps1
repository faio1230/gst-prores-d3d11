# 固定SDKで抜き出したProRes packetを変異させ、CPU境界検査をASan付きで反復する。
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/parser-asan',
    [string]$OutDir = 'results/verification-parser-mutation-2026-09-24',
    [ValidateRange(1, 100000)][int]$Iterations = 500,
    [string[]]$AdditionalInputs = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root $BuildDir
$out = Join-Path $root $OutDir
$seedDir = Join-Path $build 'seeds'
$ffmpeg = Join-Path $root 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin/ffmpeg.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
foreach ($required in @($ffmpeg, $vswhere)) {
    if (!(Test-Path -LiteralPath $required)) { throw "固定依存物がありません: $required" }
}
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
if (!$vs) { throw 'MSVC C++ビルドツールがありません' }
$major = [int]$vs.installationVersion.Split('.')[0]
$generator = switch ($major) {18 {'Visual Studio 18 2026'} 17 {'Visual Studio 17 2022'} default {throw "未検証のVisual Studio: $major"} }
$cmake = Join-Path $vs.installationPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
New-Item -ItemType Directory -Force -Path $build, $out, $seedDir | Out-Null
& $cmake -S $root -B $build -G $generator -A x64 "-DCMAKE_GENERATOR_INSTANCE=$($vs.installationPath)" '-DBUILD_GSTREAMER_PLUGIN=OFF' '-DPRORES_PARSER_ASAN=ON' *> (Join-Path $build 'configure.log')
if ($LASTEXITCODE) { throw 'ASan構成に失敗。configure.logを確認してください' }
& $cmake --build $build --config Release --target prores_parser_mutation_test *> (Join-Path $build 'build.log')
if ($LASTEXITCODE) { throw 'ASanビルドに失敗。build.logを確認してください' }
$test = Join-Path $build 'Release/prores_parser_mutation_test.exe'
if (!(Test-Path -LiteralPath $test)) { throw "テスト実行ファイルがありません: $test" }
$compiler = Get-ChildItem (Join-Path $vs.installationPath 'VC/Tools/MSVC') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$asanBin = Join-Path $compiler.FullName 'bin/Hostx64/x64'
if (!(Test-Path -LiteralPath (Join-Path $asanBin 'clang_rt.asan_dynamic-x86_64.dll'))) {
    throw 'MSVC ASan runtimeがありません'
}
$sources = @(
    'synthetic-1080p60-hq.mov',
    'synthetic-2160p60-hq.mov',
    'reference-proton-rec709-hq.mov',
    'reference-dji-nature-4k60-rec709-hq.mov'
) + $AdditionalInputs
$packets = @()
$hashes = @()
foreach ($name in $sources) {
    $source = Join-Path $root "media/$name"
    if (!(Test-Path -LiteralPath $source)) { throw "検査素材がありません: $source" }
    $packet = Join-Path $seedDir "$name.packet"
    & $ffmpeg -hide_banner -loglevel error -y -i $source -map '0:v:0' -frames:v 1 -c copy -f data $packet *> (Join-Path $build "$name.extract.log")
    if ($LASTEXITCODE) { throw "固定FFmpegによるpacket抽出に失敗: $name" }
    $packets += $packet
    $hashes += [pscustomobject]@{
        source = $name
        source_sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
        packet_sha256 = (Get-FileHash -LiteralPath $packet -Algorithm SHA256).Hash.ToLowerInvariant()
        packet_bytes = (Get-Item -LiteralPath $packet).Length
    }
}
$boundaryPacket = Join-Path $build 'ac-boundary.packet'
$arguments = @("$Iterations", $boundaryPacket) + $packets
$savedPath = $env:PATH
try {
    $env:PATH = "$asanBin;$savedPath"
    $run = & $test @arguments 2>&1
    $code = $LASTEXITCODE
} finally {
    $env:PATH = $savedPath
}
Set-Content -LiteralPath (Join-Path $out 'run.log') -Value $run -Encoding utf8
if ($code) { throw "ASan変異検査に失敗。run.logを確認してください（exit=$code）" }
$result = $run | Select-Object -Last 1 | ConvertFrom-Json
if (!$result.passed -or !$result.asan_enabled -or
    $result.seed_count -ne $sources.Count -or
    $result.cases -ne $Iterations * $sources.Count -or
    $result.boundary_rejected -lt 1 -or !(Test-Path -LiteralPath $boundaryPacket)) {
    throw 'ASan変異検査の結果が期待値と違います'
}
$originalBytes = [IO.File]::ReadAllBytes($packets[0])
$boundaryBytes = [IO.File]::ReadAllBytes($boundaryPacket)
if ($originalBytes.Length -ne $boundaryBytes.Length -or
    $originalBytes[7690] -ne 0x14 -or $boundaryBytes[7690] -ne 0x04) {
    throw 'GPU回帰用AC境界packetの素材または変異位置が変わりました'
}
$differences = 0
for ($i = 0; $i -lt $originalBytes.Length; $i++) {
    if ($originalBytes[$i] -ne $boundaryBytes[$i]) { $differences++ }
}
if ($differences -ne 1) { throw 'GPU回帰用packetが単一バイト変異ではありません' }
[pscustomobject]@{
    status = "対象$($sources.Count)素材の決定的変異・ASan検査。網羅的fuzzの保証ではない"
    result = $result
    sources = $hashes
    boundary_packet_sha256 = (Get-FileHash -LiteralPath $boundaryPacket -Algorithm SHA256).Hash.ToLowerInvariant()
    boundary_packet = (Join-Path $BuildDir 'ac-boundary.packet')
    boundary_mutation = @{ offset = 7690; original = 20; mutated = 4 }
    build = 'MSVC Release /fsanitize=address'
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
Write-Host "ProRes parser ASan変異検査成功: $($result.cases)ケース、構造受理$($result.structural_accepted)ケース"
