[CmdletBinding()]
param([switch]$IncludeFFmpeg9)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$toolsDir = Join-Path $root 'tools'
New-Item -ItemType Directory -Force $toolsDir,(Join-Path $root 'external'),(Join-Path $root 'media'),(Join-Path $root 'results') | Out-Null
$release = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-21-13-55/'
$packages = @(@{Name='ffmpeg-n8.1.3-win64-lgpl-shared-8.1.zip'; Hash='790da87e0b4c25c2063fa0306848ae4846e346c507a097b05be58310fa41d96f';Install='ffmpeg-n8.1-latest-win64-lgpl-shared-8.1';BinaryHash='61ad47c7dac1448ee609c02a04d5a9a842df8151c7fb625dad6d55d774458226'})
if ($IncludeFFmpeg9) { $packages += @{Name='ffmpeg-n9.0.2-3-ga5923073bf-win64-lgpl-shared-9.0.zip'; Hash='a7e62ca9b34c40145a2c7482f61a78063f6c8f8dbcf17effb27e62841fa6bbd9';Install='ffmpeg-n9.0-pinned-win64-lgpl-shared-9.0';BinaryHash=$null} }
foreach ($package in $packages) {
    $zip = Join-Path $toolsDir $package.Name
    if (!(Test-Path -LiteralPath $zip)) { Invoke-WebRequest ($release + $package.Name) -OutFile $zip }
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.Hash) { throw "SHA256不一致: $zip" }
    Expand-Archive -LiteralPath $zip -DestinationPath $toolsDir -Force
    $source=Join-Path $toolsDir ([IO.Path]::GetFileNameWithoutExtension($package.Name))
    $destination=Join-Path $toolsDir $package.Install
    if (!(Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath $source -Destination $destination -Recurse }
    if ($package.BinaryHash -and (Get-FileHash -LiteralPath (Join-Path $destination 'bin/ffmpeg.exe')).Hash.ToLowerInvariant() -ne $package.BinaryHash) { throw '既存SDKが固定版と異なります。別の作業ディレクトリで実行してください' }
}
$headers = Join-Path $root 'external/Vulkan-Headers'
if (!(Test-Path -LiteralPath $headers)) {
    git clone --depth 1 --branch v1.4.325 https://github.com/KhronosGroup/Vulkan-Headers.git $headers
    if ($LASTEXITCODE) { throw 'Vulkan-Headersの取得に失敗' }
}
$revision = git -C $headers rev-parse HEAD
if ($revision -ne '2e0a6e699e35c9609bde2ca4abb0d380c0378639') { throw 'Vulkan-Headersの版が想定と異なります' }
Write-Host '依存関係をプロジェクト内に準備しました。システムPATHは変更していません。'
