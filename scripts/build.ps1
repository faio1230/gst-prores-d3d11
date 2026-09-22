[CmdletBinding()]
param([string]$GStreamerRoot = 'C:/Program Files/gstreamer/1.0/msvc_x86_64')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
if (!$vs) { throw 'MSVC C++ビルドツールとWindows SDKが必要です' }
$major = [int]$vs.installationVersion.Split('.')[0]
$generator = switch ($major) {18 {'Visual Studio 18 2026'} 17 {'Visual Studio 17 2022'} default {throw "未検証のVisual Studio: $major"} }
$cmake = Join-Path $vs.installationPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake).Source }
# ディレクトリ名vs18は初回測定との互換用。VS2022でも同じ出力先を利用できる。
$build = Join-Path $root 'build/vs18'
& $cmake -S $root -B $build -G $generator -A x64 "-DCMAKE_GENERATOR_INSTANCE=$($vs.installationPath)" "-DGSTREAMER_ROOT=$GStreamerRoot"
if ($LASTEXITCODE) { throw 'CMake構成に失敗' }
& $cmake --build $build --config Release
if ($LASTEXITCODE) { throw 'ビルドに失敗' }
