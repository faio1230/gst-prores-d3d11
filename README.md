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

Windows x64, GStreamer 1.28 or later (tested with 1.28.2), a physical Direct3D 11 GPU with feature level 11_0 or later, PowerShell 7, MSVC C++ Build Tools, Windows SDK (including `fxc.exe`), CMake, and Python. Tested GPUs are an NVIDIA RTX 3070 (desktop) and an RTX 3080 Laptop GPU in a hybrid system with an AMD integrated GPU. WARP/software adapters are rejected. The build scripts use a pinned FFmpeg SDK for test tools, but the decoder plugin does not link to FFmpeg.

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

`proresd3d11dec` selects its Direct3D 11 device with `adapter` (DXGI adapter index, `-1` for the default) or `adapter-luid` (DXGI adapter LUID; a non-zero value takes precedence over `adapter`). On hybrid-GPU systems, **every** D3D11 element must use the same GPU. Otherwise D3D11 memory is copied between GPUs. Downstream elements such as `d3d11colorconvert` usually create their device before the decoder starts. If only the decoder gets `adapter-luid`, those elements stay on the default adapter, which may be the integrated GPU, and the decoder then refuses their device and creates its own on the requested GPU. Avoid this in one of two ways:
- give each D3D11 element the same `adapter` index, for example `proresd3d11dec adapter=1 ! d3d11colorconvert adapter=1`
- have the application share one device through a `GstContext`

Once the element has a device, reading `adapter-luid` returns that device's LUID, so the application can check that the GPUs match.

When decoding falls behind real time, frames already past their QoS deadline are skipped before any GPU work. GPU load then drops, so playback degrades gradually instead of collapsing.

See [build and execution](docs/ビルドと実行.md), [design](docs/設計.md), and [validation](docs/検証.md).

## Using the release binaries in an application

Each GitHub release has a zip named after its tag (for example `gst-prores-d3d11-v0.2.2-win64-gst1.28.2.zip`). When you bundle it:

- **Placement:** keep `gstproresd3d11.dll` and all `prores_*.cso` files in the same directory. Add that directory to `GST_PLUGIN_PATH`, or copy the files into GStreamer's `lib\gstreamer-1.0`. Do not rename the DLL, because GStreamer derives the plugin entry point from its file name.
- **Runtime:** the release DLL needs the Microsoft Visual C++ Redistributable x64, version 14.50 or later. GStreamer 1.28.2 does not include it.
- **Untagged streams:** color components missing from both the ProRes frame header and the input caps are filled with GStreamer's default for caps without colorimetry (BT.601 for SD, BT.709 for larger frames, or BT.2020 when a known component is BT.2020). Output caps therefore always name a full colorimetry, such as `bt709`, and every downstream converter uses the same matrix. Set colorimetry on the input caps if the material needs something else.
- **Interlaced streams:** these are output as interleaved frames with `field-order` set. They are not deinterlaced. Add a deinterlacer (such as `d3d11deinterlace`) if you need progressive frames.
- **Tested GPUs:**
  - NVIDIA RTX 3070 (desktop): an application shared its device through a `GstContext` and matched `adapter-luid`.
  - RTX 3080 Laptop GPU in a hybrid system with an AMD integrated GPU: the RTX was selected by giving the same `adapter` index to `proresd3d11dec` and `d3d11colorconvert`.
  - Not verified: decoding on AMD or Intel GPUs, and selecting the GPU on a hybrid system with `adapter-luid` plus a shared device.

`proresd3d11dec ! d3d11colorconvert ! "video/x-raw(memory:D3D11Memory),format=BGRA"` has been checked for every output format. Semi-transparent alpha also comes through that BGRA path. It is straight (not premultiplied) alpha, and it was checked with 4444 and 4444 XQ streams (8- and 16-bit alpha, 3840×2160 and 2560×1536). BGRA A differed from the `avdec_prores ! videoconvert` CPU path by at most 1, and RGB by at most 1 at p99.


## Accuracy and performance

**Accuracy:** on the tested streams, every decoded YUV pixel differed from the pinned FFmpeg CPU reference by at most one code value at the source depth. Alpha matched at the compared output depth. The native RGB converter differed from an independent BT.709 calculation by at most one RGB10A2 code value.

**Single-stream throughput** (decoding as fast as possible with `sync=false`, without GPU-completion waiting or display timing; not a playback guarantee):

| GPU | 1080p HQ | 4K HQ | 4K 4444 with alpha (about 2 Gbps) |
| --- | --- | --- | --- |
| RTX 3070 (desktop), synthetic HQ | about 436 fps | about 258 fps | – |
| RTX 3080 Laptop GPU, through `d3d11colorconvert` to BGRA | about 404 fps | about 198 fps | about 110–130 fps |

Decoding cost scales mainly with bitrate. For example, a 55 Mbps 4K 4444 stream with alpha, mostly transparent, decoded at about 227 fps on the RTX 3080 Laptop GPU. The CPU path (`avdec_prores ! videoconvert`) decoded 4K HQ at about 14 fps on a Ryzen 9 5900HX.

**Multiple streams on one GPU** (RTX 3080 Laptop GPU; N processes, each `proresd3d11dec adapter=1 ! d3d11colorconvert adapter=1 ! BGRA`; median of two runs):

- **Throughput:** the GPU's total throughput stays roughly constant and is divided among the streams. For 4K HQ it was about 170–197 fps in total from 1 to 8 streams.
- **Real time:** the largest stream count that played in real time (`sync=true qos=true`) with no dropped frames was:

| Stream | Streams with no dropped frames |
| --- | --- |
| 4K HQ 60p | 2 |
| 4K 4444 30p (about 2 Gbps) | 2–3 |
| 1080p HQ 59.94p | 4–5 |
| 1080p 4444 30p | 4 |

- **Beyond capacity:** since v0.2.2, frames at least one frame duration past their QoS deadline are skipped before any GPU work. Playback then degrades to roughly *capacity ÷ streams* instead of collapsing:

| Overload | v0.2.1 | v0.2.2 |
| --- | --- | --- |
| 4K HQ × 4 | 1.17 fps per stream, about 1 s between frames | 42.95 fps per stream, at most 67 ms between frames |
| 4K HQ × 6 | 0.67 fps per stream | 28.05 fps per stream |
| 1080p HQ × 8 | 1.41 fps per stream | 47.52 fps per stream |

Below capacity, dropped frames were the same as in v0.2.1. See [validation](docs/検証.md) for the conditions and the full results.

## Known limits

- Decoding on AMD and Intel GPUs, actual device loss/recovery, and long-running operation on other systems have not been verified.
- HDR tags are propagated, but HDR-to-RGB numerical accuracy has not been verified. The native RGB element is limited to progressive, limited BT.709.
- Odd-width 4:2:2 frames are rejected. Interlaced RGB needs a separate deinterlacer; `proresd3d11rgb` does not deinterlace.
- Corrupt frames do not stop the pipeline by default. A frame found corrupt on the CPU is dropped. A frame found corrupt by the asynchronous GPU check is reported up to three frames late, after its image has already gone downstream. Both are counted against `GstVideoDecoder`'s `max-errors` (default -1: never stop) and posted as a `STREAM/DECODE` warning. Set `max-errors` to 0 or more to stop instead. A stream that is unsupported from its first frame still stops with `STREAM/FORMAT`.
- The measured decoder QoS result does not establish OS/DWM presentation or display color accuracy. Camera-origin alpha and interlaced footage with clear redistribution rights remains untested.

## License and trademark

Licensed under LGPL-2.1-or-later. The parser and VLD shader contain work derived from FFmpeg's `proresdec.c` (and its related ProRes VLD implementation); their source files carry SPDX notices. ProRes is a trademark of Apple Inc. This project is not affiliated with Apple.
