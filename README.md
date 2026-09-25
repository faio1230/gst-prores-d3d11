# gst-prores-d3d11

[日本語](README-ja.md)

A standalone GStreamer ProRes decoder implemented with Direct3D 11 Compute Shaders. It outputs `GstD3D11Memory` without reading decoded images back to the CPU. There is no CPU, Vulkan, or FFmpeg decoding fallback. The CPU validates and prepares compressed packets; the image decoding work runs on the GPU. This is a compute implementation, not a vendor fixed-function ProRes decoder.

## Supported input and output

| Area | Support |
| --- | --- |
| Profiles | Proxy (`apco`), LT (`apcs`), Standard (`apcn`), HQ (`apch`), 4444 (`ap4h`), 4444 XQ (`ap4x`). The picture header, not the FourCC name alone, determines chroma and alpha. |
| Chroma and depth | 4:2:2 or 4:4:4; 10-bit for `apco`–`apch`, 12-bit for `ap4h`/`ap4x`. Alpha-free output: `I422_10LE`, `Y444_10LE`, `I422_12LE`, or `Y444_12LE`. |
| Alpha | None, 8-bit, or 16-bit. Alpha-bearing output is standard 16-bit `AYUV64`; 4:2:2 chroma is replicated to adjacent pixels. |
| Fields | Progressive, top-field-first, and bottom-field-first interlaced frames. Deinterlacing is a downstream operation. |
| Color | BT.601, BT.709, BT.2020, PQ, and HLG tags are propagated to standard output caps. `proresd3d11rgb` accepts progressive, limited-range BT.709 only. |
| Dimensions and changes | Non-multiples of 16 are supported; 4:4:4 may have odd width. 4:2:2 requires even width. Color, dimensions, and output format can change within one stream with caps renegotiation. |

See the [feature table](docs/機能対応表.md) for the tested combinations and precise limits.

## Requirements

Windows x64, GStreamer 1.28 or later (tested with 1.28.2), a physical Direct3D 11 GPU with feature level 11_0 or later, PowerShell 7, MSVC C++ Build Tools, Windows SDK (including `fxc.exe`), CMake, and Python. Only an NVIDIA RTX 3070 has been tested; WARP/software adapters are rejected. The build scripts use a pinned FFmpeg SDK for test tools, but the decoder plugin does not link to FFmpeg.

## Build and run

In PowerShell 7, from the repository root:

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1
. ./scripts/use-d3d11-plugin.ps1
```

`use-d3d11-plugin.ps1` configures the current shell only. Pass `-GStreamerRoot` to the build and setup scripts if GStreamer is not at their default MSVC x64 location. Replace `sample.mov` below with a ProRes MOV file:

```powershell
# Decode to D3D11Memory without a display conversion.
gst-launch-1.0 -e filesrc location=sample.mov ! qtdemux ! proresd3d11dec ! "video/x-raw(memory:D3D11Memory)" ! fakesink sync=false

# Progressive, limited BT.709: GPU RGB conversion and display.
gst-launch-1.0 -e filesrc location=sample.mov ! qtdemux ! proresd3d11dec ! proresd3d11rgb ! d3d11videosink

# Alpha-bearing ProRes: pass standard AYUV64 directly to the sink.
gst-launch-1.0 -e filesrc location=sample.mov ! qtdemux ! proresd3d11dec ! "video/x-raw(memory:D3D11Memory),format=AYUV64" ! d3d11videosink
```

`proresd3d11dec` selects its Direct3D 11 device with `adapter` (DXGI adapter index, `-1` for the default) or `adapter-luid` (DXGI adapter LUID; a non-zero value takes precedence over `adapter`). On hybrid-GPU systems, set `adapter-luid` so the decoder uses the same GPU as the rest of the pipeline. Once the element has a device, reading `adapter-luid` returns that device's LUID.

See [build and execution](docs/ビルドと実行.md), [design](docs/設計.md), and [validation](docs/検証.md).

## Accuracy and performance

On the tested streams, every decoded YUV pixel differed from the pinned FFmpeg CPU reference by at most one code value at the source depth; alpha matched at the compared output depth. The native RGB converter differed from an independent BT.709 calculation by at most one RGB10A2 code value. On an RTX 3070, synthetic HQ direct-output throughput was about **436 fps at 1080p** and **258 fps at 4K**. These are decoder throughput measurements without GPU-completion waiting or display timing, not playback guarantees.

## Known limits

- AMD and Intel GPUs, actual device loss/recovery, and long-running operation on other systems have not been verified.
- HDR tags are propagated, but HDR-to-RGB numerical accuracy has not been verified. The native RGB element is limited to progressive, limited BT.709.
- Odd-width 4:2:2 frames are rejected. Interlaced RGB needs a separate deinterlacer; `proresd3d11rgb` does not deinterlace.
- Corrupt bitstream errors may be reported up to three frames late by the asynchronous GPU error check; an affected image may reach downstream first.
- The measured decoder QoS result does not establish OS/DWM presentation or display color accuracy. Camera-origin alpha and interlaced footage with clear redistribution rights remains untested.

## License and trademark

Licensed under LGPL-2.1-or-later. The parser and VLD shader contain work derived from FFmpeg's `proresdec.c` (and its related ProRes VLD implementation); their source files carry SPDX notices. ProRes is a trademark of Apple Inc. This project is not affiliated with Apple.
