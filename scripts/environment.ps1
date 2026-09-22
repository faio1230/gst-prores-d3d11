$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'results'
New-Item -ItemType Directory -Force $out | Out-Null
$envReport=@{
    collected_utc=(Get-Date).ToUniversalTime().ToString('o')
    os=Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,TotalVisibleMemorySize
    cpu=Get-CimInstance Win32_Processor | Select-Object Name,NumberOfLogicalProcessors
    gpu=Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion
    commands=Get-Command ffmpeg,ffprobe,gst-inspect-1.0,vulkaninfo,cmake,python -ErrorAction SilentlyContinue | Select-Object Name,Source
}
$envReport | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'environment.json') -Encoding utf8
vulkaninfo 2> (Join-Path $out 'vulkaninfo-stderr.txt') | Out-File (Join-Path $out 'vulkaninfo.txt') -Encoding utf8
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv | Out-File (Join-Path $out 'nvidia-environment.csv') -Encoding utf8
gst-inspect-1.0 avdec_prores | Out-File (Join-Path $out 'gst-avdec-prores.txt') -Encoding utf8
gst-inspect-1.0 d3d11 | Out-File (Join-Path $out 'gst-d3d11.txt') -Encoding utf8
