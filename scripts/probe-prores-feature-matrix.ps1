# 固定FFmpeg SDKのProRes CPU/Vulkan経路を同じ合成入力で測る。復号器本体は変更しない。
param(
    [string]$Sdk = 'tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1/bin',
    [string]$OutDir = 'results/feature-matrix-2026-09-24'
)

$ErrorActionPreference = 'Stop'
$ffmpeg = Join-Path $Sdk 'ffmpeg.exe'
$ffprobe = Join-Path $Sdk 'ffprobe.exe'
if (-not (Test-Path -LiteralPath $ffmpeg) -or -not (Test-Path -LiteralPath $ffprobe)) {
    throw "固定SDKがありません: $Sdk"
}
$root = (Get-Location).Path
$out = Join-Path $root $OutDir
$media = Join-Path $root 'media/feature-matrix-2026-09-24'
New-Item -ItemType Directory -Force -Path $out, $media | Out-Null

$cases = @(
    @{ Name='apco'; Profile=0; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le' },
    @{ Name='apcs'; Profile=1; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le' },
    @{ Name='apcn'; Profile=2; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le' },
    @{ Name='apch'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le' },
    @{ Name='apco-444'; Profile=0; Alpha=0; Format='yuv444p10le'; Download='yuv444p10le' },
    @{ Name='apcs-444'; Profile=1; Alpha=0; Format='yuv444p10le'; Download='yuv444p10le' },
    @{ Name='apcn-444'; Profile=2; Alpha=0; Format='yuv444p10le'; Download='yuv444p10le' },
    @{ Name='apch-444'; Profile=3; Alpha=0; Format='yuv444p10le'; Download='yuv444p10le' },
    @{ Name='ap4h-422'; Profile=4; Alpha=0; Format='yuv422p10le'; Download='yuv422p12le' },
    @{ Name='ap4x-422'; Profile=5; Alpha=0; Format='yuv422p10le'; Download='yuv422p12le' },
    @{ Name='ap4h-no-alpha'; Profile=4; Alpha=0; Format='yuv444p10le'; Download='yuv444p12le' },
    @{ Name='ap4h-alpha8'; Profile=4; Alpha=8; Format='yuva444p10le'; Download='yuva444p12le' },
    @{ Name='ap4h-alpha16'; Profile=4; Alpha=16; Format='yuva444p10le'; Download='yuva444p12le' },
    @{ Name='ap4x-no-alpha'; Profile=5; Alpha=0; Format='yuv444p10le'; Download='yuv444p12le' },
    @{ Name='ap4x-alpha8'; Profile=5; Alpha=8; Format='yuva444p10le'; Download='yuva444p12le' },
    @{ Name='ap4x-alpha16'; Profile=5; Alpha=16; Format='yuva444p10le'; Download='yuva444p12le' },
    @{ Name='apch-tff'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Field='tff' },
    @{ Name='apch-bff'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Field='bff' },
    @{ Name='ap4h-tff-alpha16'; Profile=4; Alpha=16; Format='yuva444p10le'; Download='yuva444p12le'; Field='tff' },
    @{ Name='ap4h-bff-alpha16'; Profile=4; Alpha=16; Format='yuva444p10le'; Download='yuva444p12le'; Field='bff' },
    @{ Name='apch-bt601'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Color='smpte170m' },
    @{ Name='apch-bt709'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Color='bt709' },
    @{ Name='apch-bt2020'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Color='bt2020' },
    @{ Name='apch-pq'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Color='pq' },
    @{ Name='apch-hlg'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Color='hlg' },
    @{ Name='apch-non16'; Profile=3; Alpha=0; Format='yuv422p10le'; Download='yuv422p10le'; Size='318x178' },
    @{ Name='ap4h-non16'; Profile=4; Alpha=0; Format='yuv444p10le'; Download='yuv444p12le'; Size='319x179'; Source='testsrc' }
)

$colors = @{
    smpte170m = 'color_primaries=smpte170m:color_trc=smpte170m:colorspace=smpte170m'
    bt709 = 'color_primaries=bt709:color_trc=bt709:colorspace=bt709'
    bt2020 = 'color_primaries=bt2020:color_trc=bt709:colorspace=bt2020nc'
    pq = 'color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc'
    hlg = 'color_primaries=bt2020:color_trc=arib-std-b67:colorspace=bt2020nc'
}
$version = (& $ffmpeg -version | Select-Object -First 1)
$rows = @()
foreach ($case in $cases) {
    $size = if ($case.Size) { $case.Size } else { '320x180' }
    $filter = "format=$($case.Format)"
    if ($case.Field) { $filter += ",setfield=$($case.Field)" }
    if ($case.Color) { $filter += ",setparams=$($colors[$case.Color])" }
    $file = Join-Path $media "$($case.Name).mov"
    $base = Join-Path $out $case.Name
    $source = if ($case.Source) { $case.Source } else { 'testsrc2' }
    $encodeArgs = @('-hide_banner','-y','-loglevel','error','-f','lavfi','-i',"${source}=size=$($size):rate=30",'-frames:v','2','-vf',$filter)
    if ($case.Field) {
        $order = if ($case.Field -eq 'tff') { 'tt' } else { 'bb' }
        $encodeArgs += @('-flags','+ildct','-field_order',$order)
    }
    $encodeArgs += @('-c:v','prores_ks','-profile:v',[string]$case.Profile,'-alpha_bits',[string]$case.Alpha,'-movflags','+write_colr',$file)
    & $ffmpeg @encodeArgs 2>&1 | Out-File -LiteralPath "$base-encode.log" -Encoding utf8
    $encodeExit = $LASTEXITCODE
    if ($encodeExit -ne 0) {
        $rows += [pscustomobject]@{ name=$case.Name; encodeExit=$encodeExit; vulkanOk=$false }
        Write-Host "$($case.Name): encode failed ($encodeExit)"
        continue
    }
    $probe = (& $ffprobe -v error -select_streams v:0 -show_entries 'stream=codec_tag_string,pix_fmt,width,height,field_order,color_space,color_transfer,color_primaries' -of json $file | ConvertFrom-Json).streams[0]
    $cpuMd5 = "$base-cpu.framemd5"
    & $ffmpeg -hide_banner -y -loglevel error -i $file -frames:v 2 -pix_fmt $case.Download -f framemd5 $cpuMd5 2>&1 | Out-File -LiteralPath "$base-cpu.log" -Encoding utf8
    $cpuExit = $LASTEXITCODE
    $vkMd5 = "$base-vulkan.framemd5"
    & $ffmpeg -hide_banner -y -loglevel verbose -init_hw_device vulkan=vk:0 -filter_hw_device vk -hwaccel vulkan -hwaccel_output_format vulkan -i $file -frames:v 2 -vf "hwdownload,format=$($case.Download)" -f framemd5 $vkMd5 2>&1 | Out-File -LiteralPath "$base-vulkan.log" -Encoding utf8
    $vulkanExit = $LASTEXITCODE
    $vkLog = Get-Content -LiteralPath "$base-vulkan.log" -Raw
    $gpuFrame = $vkLog -match 'pixfmt:vulkan'
    $gpuName = $vkLog -match 'Device 0 selected: NVIDIA GeForce RTX 3070'
    $frameCount = if (Test-Path -LiteralPath $vkMd5) { @(Get-Content -LiteralPath $vkMd5 | Where-Object { $_ -match '^\d+,' }).Count } else { 0 }
    $row = [pscustomobject]@{
        name=$case.Name; source="media/feature-matrix-2026-09-24/$($case.Name).mov"; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        fourcc=$probe.codec_tag_string; pixFmt=$probe.pix_fmt; width=$probe.width; height=$probe.height
        fieldOrder=$probe.field_order; colorSpace=$probe.color_space; transfer=$probe.color_transfer; primaries=$probe.color_primaries
        alphaBits=$case.Alpha; cpuExit=$cpuExit; vulkanExit=$vulkanExit; gpuFrame=$gpuFrame; rtx3070=$gpuName
        frameCount=$frameCount; vulkanOk=($vulkanExit -eq 0 -and $gpuFrame -and $gpuName -and $frameCount -eq 2)
    }
    $rows += $row
    Write-Host "$($case.Name): CPU=$cpuExit Vulkan=$($row.vulkanOk) frames=$frameCount"
}
# prores_ksは422系FourCCでalphaを直接符号化しない。4444の有効packetを
# 四CCだけ差し替え、decoderが10bit/444/alphaとして扱う経路を別途測る。
foreach ($tag in @('apco','apcs','apcn','apch')) {
    foreach ($alpha in @(8,16)) {
        $name = "$tag-444-alpha$alpha-retag"
        $base = Join-Path $out $name
        $file = Join-Path $media "$name.mov"
        $source = Join-Path $media "ap4h-alpha$alpha.mov"
        & $ffmpeg -hide_banner -y -loglevel error -i $source -c:v copy -tag:v $tag $file 2>&1 |
            Out-File -LiteralPath "$base-retag.log" -Encoding utf8
        $retagExit = $LASTEXITCODE
        if ($retagExit -ne 0) {
            $rows += [pscustomobject]@{ name=$name; encodeExit=$retagExit; vulkanOk=$false }
            continue
        }
        $probe = (& $ffprobe -v error -select_streams v:0 -show_entries 'stream=codec_tag_string,pix_fmt,width,height,field_order' -of json $file | ConvertFrom-Json).streams[0]
        & $ffmpeg -hide_banner -y -loglevel error -i $file -frames:v 2 -pix_fmt yuva444p10le -f framemd5 "$base-cpu.framemd5" 2>&1 |
            Out-File -LiteralPath "$base-cpu.log" -Encoding utf8
        $cpuExit = $LASTEXITCODE
        & $ffmpeg -hide_banner -y -loglevel verbose -init_hw_device vulkan=vk:0 -filter_hw_device vk -hwaccel vulkan -hwaccel_output_format vulkan -i $file -frames:v 2 -vf 'hwdownload,format=yuva444p10le' -f framemd5 "$base-vulkan.framemd5" 2>&1 |
            Out-File -LiteralPath "$base-vulkan.log" -Encoding utf8
        $vulkanExit = $LASTEXITCODE
        $vkLog = Get-Content -LiteralPath "$base-vulkan.log" -Raw
        $gpuFrame = $vkLog -match 'pixfmt:vulkan'
        $gpuName = $vkLog -match 'Device 0 selected: NVIDIA GeForce RTX 3070'
        $frameCount = if (Test-Path -LiteralPath "$base-vulkan.framemd5") {
            @(Get-Content -LiteralPath "$base-vulkan.framemd5" | Where-Object { $_ -match '^\d+,' }).Count
        } else { 0 }
        $row = [pscustomobject]@{
            name=$name; source="media/feature-matrix-2026-09-24/$name.mov"; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
            fourcc=$probe.codec_tag_string; pixFmt=$probe.pix_fmt; width=$probe.width; height=$probe.height
            fieldOrder=$probe.field_order; alphaBits=$alpha; method='ap4h packetを再符号化せずFourCCのみ変更'
            cpuExit=$cpuExit; vulkanExit=$vulkanExit; gpuFrame=$gpuFrame; rtx3070=$gpuName
            frameCount=$frameCount; vulkanOk=($vulkanExit -eq 0 -and $gpuFrame -and $gpuName -and $frameCount -eq 2)
        }
        $rows += $row
        Write-Host "$name`: CPU=$cpuExit Vulkan=$($row.vulkanOk) frames=$frameCount"
    }
}
$dynamic = @()
foreach ($kind in @('resolution','format')) {
    $list = Join-Path $root "scripts/fixtures/prores-m0-$kind.ffconcat"
    $expected = if ($kind -eq 'resolution') { 4 } else { 6 }
    & $ffmpeg -hide_banner -loglevel error -f concat -safe 0 -i $list -noautoscale -c:v wrapped_avframe -f null NUL 2>&1 |
        Out-File -LiteralPath (Join-Path $out "dynamic-$kind-cpu.log") -Encoding utf8
    $cpuExit = $LASTEXITCODE
    $vkLogPath = Join-Path $out "dynamic-$kind-vulkan.log"
    & $ffmpeg -hide_banner -loglevel verbose -init_hw_device vulkan=vk:0 -filter_hw_device vk -hwaccel vulkan -hwaccel_output_format vulkan -f concat -safe 0 -i $list -noautoscale -c:v wrapped_avframe -f null NUL 2>&1 |
        Out-File -LiteralPath $vkLogPath -Encoding utf8
    $vkExit = $LASTEXITCODE
    $vkLog = Get-Content -LiteralPath $vkLogPath -Raw
    $gpuFrames = @([regex]::Matches($vkLog, 'pixfmt:vulkan')).Count
    $encoded = [regex]::Matches($vkLog, '(\d+) frames encoded')
    $count = if ($encoded.Count) { [int]$encoded[$encoded.Count - 1].Groups[1].Value } else { 0 }
    $row = [pscustomobject]@{
        name=$kind; cpuExit=$cpuExit; vulkanExit=$vkExit; vulkanGraphCount=$gpuFrames
        frameCount=$count; vulkanOk=($vkExit -eq 0 -and $gpuFrames -ge 2 -and $count -eq $expected)
        method='-noautoscale -c:v wrapped_avframe -f null NUL; 動的形式ではGPU画像のreadbackなし'
    }
    $dynamic += $row
    Write-Host "dynamic-$kind`: CPU=$cpuExit Vulkan=$($row.vulkanOk) frames=$count"
}
[pscustomobject]@{ sdk=$ffmpeg; version=$version; gpu='NVIDIA GeForce RTX 3070'; cases=$rows; dynamic=$dynamic } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'summary.json') -Encoding utf8
if (@($rows | Where-Object { $_.encodeExit -or -not $_.vulkanOk }).Count -gt 0 -or
    @($dynamic | Where-Object { $_.cpuExit -ne 0 -or -not $_.vulkanOk }).Count -gt 0) { exit 1 }
