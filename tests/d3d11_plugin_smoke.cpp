// 純粋DX11 GStreamer要素のD3D11Memory、時刻、seek、寿命、失敗入力を検証する。
#include "d3d11_hardware_device.hpp"
#include "prores_parser.hpp"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11bufferpool.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/d3d11/gstd3d11utils.h>
#include <gst/gst.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct DecoderGpuCapabilities {
    unsigned feature_level;
    UINT r16_support;
};

static DecoderGpuCapabilities check_device_capabilities() {
    auto* gst_device = gst_d3d11_device_new(0, 0);
    require(gst_device != nullptr, "cannot create D3D11 capability test device");
    auto* device = gst_d3d11_device_get_device_handle(gst_device);
    require(device != nullptr, "D3D11 capability test has no native device");
    const auto level = device->GetFeatureLevel();
    UINT support = 0;
    const HRESULT result = device->CheckFormatSupport(DXGI_FORMAT_R16_UNORM, &support);
    gst_object_unref(gst_device);
    require(level >= D3D_FEATURE_LEVEL_11_0, "GPU does not support required SM5 feature level");
    require(SUCCEEDED(result), "R16_UNORM format support query failed");
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
        D3D11_FORMAT_SUPPORT_SHADER_LOAD |
        D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    require((support & required) == required, "R16_UNORM texture/load/typed UAV capability missing");
    return {static_cast<unsigned>(level), support};
}

struct Pipeline {
    GstElement* pipe = nullptr;
    GstElement* sink = nullptr;
    GstBus* bus = nullptr;

    explicit Pipeline(const char* description) {
        GError* error = nullptr;
        pipe = gst_parse_launch(description, &error);
        if (error) {
            std::string message = error->message;
            g_error_free(error);
            if (pipe) gst_object_unref(pipe);
            throw std::runtime_error(message);
        }
        require(pipe != nullptr, "pipeline missing");
        sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
        bus = gst_element_get_bus(pipe);
        if (auto* demux = gst_bin_get_by_name(GST_BIN(pipe), "demux")) {
            g_signal_connect(demux, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad,
                                                                  gpointer data) {
                auto* target = gst_bin_get_by_name(GST_BIN(data), "decoder");
                auto* input = gst_element_get_static_pad(target, "sink");
                if (!gst_pad_is_linked(input) && gst_pad_link(pad, input) != GST_PAD_LINK_OK)
                    GST_ELEMENT_ERROR(target, CORE, PAD, ("Test demux link failed"), (nullptr));
                gst_object_unref(input);
                gst_object_unref(target);
            }), pipe);
            gst_object_unref(demux);
        }
    }

    ~Pipeline() {
        gst_element_set_state(pipe, GST_STATE_NULL);
        if (sink) gst_object_unref(sink);
        gst_object_unref(bus);
        gst_object_unref(pipe);
    }

    void file(const char* path) {
        auto* source = gst_bin_get_by_name(GST_BIN(pipe), "source");
        require(source != nullptr, "source missing");
        g_object_set(source, "location", path, nullptr);
        gst_object_unref(source);
    }

    void state(GstState target) {
        require(gst_element_set_state(pipe, target) != GST_STATE_CHANGE_FAILURE,
                "state transition failed");
    }

    void errors() {
        auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (!message) return;
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::string text = error ? error->message : "unknown bus error";
        if (debug) text += std::string(" : ") + debug;
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        throw std::runtime_error(text);
    }

    GstSample* pull() {
        auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        errors();
        if (!sample) require(gst_app_sink_is_eos(GST_APP_SINK(sink)), "sample timeout");
        return sample;
    }

    void expect_error(GQuark expected_domain = 0, int expected_code = -1,
                      const char* expected_debug_text = nullptr) {
        auto* message = gst_bus_timed_pop_filtered(bus, 15 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        require(message != nullptr, "expected error timed out");
        const bool is_error = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
        if (is_error) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            require(error != nullptr, "error message has no GError");
            std::cerr << "expected_error=" << error->message << '\n';
            const bool expected_type =
                (!expected_domain || error->domain == expected_domain) &&
                (expected_code < 0 || error->code == expected_code);
            const bool expected_debug = !expected_debug_text ||
                (debug && std::strstr(debug, expected_debug_text));
            g_error_free(error);
            g_free(debug);
            gst_message_unref(message);
            require(expected_type, "unexpected error domain or code");
            require(expected_debug, "expected error detail missing");
            return;
        }
        gst_message_unref(message);
        require(is_error, "unexpected successful EOS");
    }
};

static constexpr auto direct_pipeline =
    "filesrc name=source ! qtdemux name=demux proresd3d11dec name=decoder ! "
    "appsink name=sink sync=false max-buffers=4";

static void check_caps(GstSample* sample, bool full_bt709) {
    auto* caps = gst_sample_get_caps(sample);
    GstVideoInfo info{};
    require(gst_video_info_from_caps(&info, caps), "invalid video caps");
    require(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_I422_10LE,
            "unexpected pixel format");
    require(info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235 &&
            info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709 &&
            info.colorimetry.primaries == (full_bt709 ? GST_VIDEO_COLOR_PRIMARIES_BT709
                                                       : GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) &&
            info.colorimetry.transfer == (full_bt709 ? GST_VIDEO_TRANSFER_BT709
                                                      : GST_VIDEO_TRANSFER_UNKNOWN),
            "color metadata mismatch");
}

static void check_d3d_sample(GstSample* sample, GstClockTime pts, bool full_bt709 = false,
                             int width = 1920, int height = 1080,
                             const GstVideoColorimetry* expected_color = nullptr) {
    auto* buffer = gst_sample_get_buffer(sample);
    require(GST_BUFFER_PTS(buffer) == pts, "PTS mismatch");
    require(GST_BUFFER_DURATION(buffer) > 0 &&
            GST_BUFFER_DURATION(buffer) != GST_CLOCK_TIME_NONE, "duration missing");
    if (!expected_color) check_caps(sample, full_bt709);
    auto* caps = gst_sample_get_caps(sample);
    GstVideoInfo info{};
    require(gst_video_info_from_caps(&info, caps) &&
            GST_VIDEO_INFO_WIDTH(&info) == width && GST_VIDEO_INFO_HEIGHT(&info) == height,
            "D3D11 output caps dimensions mismatch");
    if (expected_color)
        require(info.colorimetry.range == expected_color->range &&
                info.colorimetry.matrix == expected_color->matrix &&
                info.colorimetry.primaries == expected_color->primaries &&
                info.colorimetry.transfer == expected_color->transfer,
                "dynamic color caps mismatch");
    require(gst_caps_features_contains(gst_caps_get_features(caps, 0),
                                       GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY),
            "D3D11Memory caps feature missing");
    require(gst_buffer_n_memory(buffer) == 3, "D3D11 output must have three memories");
    for (guint component = 0; component < 3; ++component) {
        auto* memory = gst_buffer_peek_memory(buffer, component);
        require(gst_is_d3d11_memory(memory), "non-D3D11 output memory");
        D3D11_TEXTURE2D_DESC desc{};
        require(gst_d3d11_memory_get_texture_desc(GST_D3D11_MEMORY_CAST(memory), &desc),
                "texture description unavailable");
        require(desc.Format == DXGI_FORMAT_R16_UNORM &&
                (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0,
                "unexpected output texture format or bind flags");
        require(desc.Width == static_cast<UINT>(component ? width / 2 : width) &&
                desc.Height == static_cast<UINT>(height),
                "unexpected output texture size");
    }
}

static void check_same_pixels(const GstVideoInfo& info, GstBuffer* before_buffer,
                              GstBuffer* after_buffer, const char* failure) {
    GstVideoFrame before{}, after{};
    require(gst_video_frame_map(&before, &info, before_buffer, GST_MAP_READ),
            "first D3D11 frame readback failed");
    if (!gst_video_frame_map(&after, &info, after_buffer, GST_MAP_READ)) {
        gst_video_frame_unmap(&before);
        require(false, "final D3D11 frame readback failed");
    }
    bool equal = true;
    for (guint component = 0; component < 3 && equal; ++component) {
        const auto row_bytes = static_cast<std::size_t>(component ? info.width : info.width * 2);
        for (int y = 0; y < info.height; ++y) {
            const auto* before_row = static_cast<const guint8*>(
                GST_VIDEO_FRAME_PLANE_DATA(&before, component)) +
                y * GST_VIDEO_FRAME_PLANE_STRIDE(&before, component);
            const auto* after_row = static_cast<const guint8*>(
                GST_VIDEO_FRAME_PLANE_DATA(&after, component)) +
                y * GST_VIDEO_FRAME_PLANE_STRIDE(&after, component);
            if (std::memcmp(before_row, after_row, row_bytes) != 0) {
                equal = false;
                break;
            }
        }
    }
    gst_video_frame_unmap(&after);
    gst_video_frame_unmap(&before);
    require(equal, failure);
}

static void check_downloaded_sample(GstSample* sample) {
    check_caps(sample, false);
    auto* caps = gst_sample_get_caps(sample);
    require(!gst_caps_features_contains(gst_caps_get_features(caps, 0),
                                        GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY),
            "download still advertises D3D11Memory");
    GstVideoInfo info{};
    require(gst_video_info_from_caps(&info, caps), "invalid downloaded caps");
    GstVideoFrame frame{};
    require(gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ),
            "downloaded frame mapping failed");
    bool nonzero = false;
    for (int y = 0; y < info.height && !nonzero; ++y) {
        const auto* row = static_cast<const guint16*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)) +
                          y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0) / 2;
        for (int x = 0; x < info.width; ++x) {
            if (row[x] != 0) { nonzero = true; break; }
        }
    }
    gst_video_frame_unmap(&frame);
    require(nonzero, "all-zero downloaded luma plane");
}

static void injected_error(GstSample* compressed, const char* test) {
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! fakesink");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    auto* caps = gst_caps_copy(gst_sample_get_caps(compressed));
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    guint8 byte = 0;
    std::string expected_gpu_error = "GPU entropy decoder rejected job";
    if (std::string(test) == "bad-signature") gst_buffer_fill(input, 4, &byte, 1);
    if (std::string(test) == "alpha-hidden-in-caps") {
        byte = 2;
        gst_buffer_fill(input, 25, &byte, 1);
    }
    if (std::string(test) == "invalid-alpha-mode") {
        byte = 3;
        gst_buffer_fill(input, 25, &byte, 1);
    }
    if (std::string(test) == "interlaced-hidden-in-caps") {
        gst_buffer_extract(input, 20, &byte, 1);
        byte |= 4;
        gst_buffer_fill(input, 20, &byte, 1);
    }
    if (std::string(test) == "invalid-chroma-flags") {
        gst_buffer_extract(input, 20, &byte, 1);
        byte &= static_cast<guint8>(~0xc0);
        gst_buffer_fill(input, 20, &byte, 1);
    }
    if (std::string(test) == "oversized-first-dc") {
        GstMapInfo mapped{};
        require(gst_buffer_map(input, &mapped, GST_MAP_READ), "cannot map entropy mutation input");
        prores::Frame parsed;
        std::string parse_error;
        const bool valid = prores::parse_frame(mapped.data, mapped.size, 0, 0,
                                                parsed, parse_error);
        gst_buffer_unmap(input, &mapped);
        require(valid && !parsed.slices.empty() && parsed.slices[0].planes[0].size >= 4,
                "entropy mutation fixture has no first luma plane");
        const guint8 oversized_dc[] = {0x00, 0x1f, 0xff, 0xf0};
        require(gst_buffer_fill(input, parsed.slices[0].planes[0].offset,
                                oversized_dc, sizeof(oversized_dc)) == sizeof(oversized_dc),
                "cannot write oversized first DC code");
    }
    if (std::string(test) == "ac-run-boundary") {
        // 固定1080p素材の先頭packet: ASan変異検査で見つけた単一byteの再現例。
        constexpr gsize offset = 7690;
        require(gst_buffer_extract(input, offset, &byte, 1) == 1 && byte == 0x14,
                "AC boundary fixture byte changed");
        byte = 0x04;
        require(gst_buffer_fill(input, offset, &byte, 1) == 1,
                "cannot write AC boundary mutation");
        GstMapInfo mapped{};
        require(gst_buffer_map(input, &mapped, GST_MAP_READ), "cannot map AC boundary input");
        prores::Frame parsed;
        std::string parse_error;
        const bool valid = prores::parse_frame(mapped.data, mapped.size, 0, 0,
                                                parsed, parse_error);
        std::vector<prores::CoefficientJob> jobs;
        std::vector<std::int32_t> coefficients;
        const bool entropy_valid = valid && prores::make_coefficient_reference(
            mapped.data, mapped.size, parsed, jobs, coefficients, parse_error);
        gst_buffer_unmap(input, &mapped);
        require(valid && !entropy_valid &&
                parse_error.find("AC run reaches coefficient plane end") != std::string::npos,
                "AC boundary mutation did not reach the target CPU check");
        unsigned slice = 0, component = 0;
        require(sscanf_s(parse_error.c_str(), "slice %u component %u:",
                         &slice, &component) == 2 && component < 3,
                "AC boundary CPU job could not be identified");
        expected_gpu_error += " " + std::to_string(slice * 3 + component);
    }
    pipeline.state(GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(source), input);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    if (std::string(test) == "invalid-alpha-mode")
        pipeline.expect_error(GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT);
    else if (std::string(test) == "oversized-first-dc" ||
        std::string(test) == "ac-run-boundary")
        pipeline.expect_error(GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE,
                              expected_gpu_error.c_str());
    else
        pipeline.expect_error();
}

static void delayed_entropy_error(GstSample* compressed) {
    // EOSを送らず4枚目のring再利用で、1枚目のGPUエラーが伝播することを確認する。
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! fakesink");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(compressed));
    pipeline.state(GST_STATE_PLAYING);
    for (guint i = 0; i < 4; ++i) {
        auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
        GST_BUFFER_PTS(input) = i * GST_SECOND / 60;
        if (i == 0) {
            GstMapInfo mapped{};
            require(gst_buffer_map(input, &mapped, GST_MAP_READ), "cannot map delayed mutation");
            prores::Frame parsed;
            std::string parse_error;
            const bool valid = prores::parse_frame(mapped.data, mapped.size, 0, 0,
                                                    parsed, parse_error);
            gst_buffer_unmap(input, &mapped);
            require(valid && !parsed.slices.empty(), "delayed mutation fixture invalid");
            const guint8 oversized_dc[] = {0x00, 0x1f, 0xff, 0xf0};
            require(gst_buffer_fill(input, parsed.slices[0].planes[0].offset,
                                    oversized_dc, sizeof(oversized_dc)) == sizeof(oversized_dc),
                    "cannot write delayed entropy mutation");
        }
        gst_app_src_push_buffer(GST_APP_SRC(source), input);
    }
    gst_object_unref(source);
    pipeline.expect_error(GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE,
                          "GPU entropy decoder rejected job 0 frame=0");
}

static void alpha_entropy_error(GstSample* compressed) {
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! fakesink");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(compressed));
    auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    GstMapInfo mapped{};
    require(gst_buffer_map(input, &mapped, GST_MAP_READ), "cannot map alpha mutation input");
    prores::Frame parsed;
    std::string error;
    const bool valid = prores::parse_frame(mapped.data, mapped.size, 0, 0,
                                           parsed, error, 12, true);
    gst_buffer_unmap(input, &mapped);
    require(valid && parsed.alpha_info && !parsed.slices.empty() &&
            parsed.slices[0].planes[3].size > 0,
            "alpha mutation fixture has no alpha payload");
    const auto& alpha = parsed.slices[0].planes[3];
    require(gst_buffer_memset(input, alpha.offset, 0, alpha.size) == alpha.size,
            "cannot zero alpha payload");
    pipeline.state(GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(source), input);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    pipeline.expect_error(GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE,
                          "GPU entropy decoder rejected job");
    pipeline.state(GST_STATE_NULL);

    // EOSなしで3スロットを使い切り、4枚目の投入時に初回alpha異常を伝播する。
    Pipeline delayed("appsrc name=source format=time ! proresd3d11dec ! fakesink");
    auto* delayed_source = gst_bin_get_by_name(GST_BIN(delayed.pipe), "source");
    gst_app_src_set_caps(GST_APP_SRC(delayed_source), gst_sample_get_caps(compressed));
    delayed.state(GST_STATE_PLAYING);
    for (guint i = 0; i < 4; ++i) {
        auto* packet = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
        GST_BUFFER_PTS(packet) = i * GST_SECOND / 30;
        if (i == 0)
            require(gst_buffer_memset(packet, alpha.offset, 0, alpha.size) == alpha.size,
                    "cannot zero delayed alpha payload");
        gst_app_src_push_buffer(GST_APP_SRC(delayed_source), packet);
    }
    gst_object_unref(delayed_source);
    const auto expected = "GPU entropy decoder rejected job " +
        std::to_string(parsed.slices.size() * 3) + " frame=0";
    delayed.expect_error(GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE, expected.c_str());
}

static void reject_software_device(GstSample* compressed) {
    Microsoft::WRL::ComPtr<ID3D11Device> native;
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actual{};
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                        requested, 1, D3D11_SDK_VERSION, &native,
                                        &actual, nullptr)), "cannot create WARP test device");
    require(actual >= D3D_FEATURE_LEVEL_11_0, "WARP test device lacks SM5");
    const auto description = d3d11_adapter_description(native.Get());
    require((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0,
            "WARP test adapter does not report software flag");
    auto* wrapped = gst_d3d11_device_new_wrapped(native.Get());
    require(wrapped != nullptr, "cannot wrap WARP test device for GStreamer");
    {
        Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec name=decoder ! "
                          "fakesink name=sink");
        auto* context = gst_d3d11_context_new(wrapped);
        require(context != nullptr, "cannot create WARP GStreamer context");
        gst_element_set_context(pipeline.pipe, context);
        gst_context_unref(context);
        auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
        require(source != nullptr, "WARP decoder appsrc missing");
        gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(compressed));
        gst_element_set_state(pipeline.pipe, GST_STATE_PLAYING);
        auto* packet = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
        require(packet != nullptr, "WARP test packet copy failed");
        gst_app_src_push_buffer(GST_APP_SRC(source), packet);
        gst_object_unref(source);
        pipeline.expect_error(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
                              "software D3D11 adapter");
    }

    auto* caps = gst_caps_from_string(
        "video/x-raw(memory:D3D11Memory),format=I422_10LE,width=16,height=16,"
        "framerate=60/1,interlace-mode=progressive,colorimetry=bt709,chroma-site=jpeg");
    GstVideoInfo info{};
    require(caps && gst_video_info_from_caps(&info, caps), "WARP RGB caps invalid");
    auto* pool = gst_d3d11_buffer_pool_new(wrapped);
    require(pool != nullptr, "cannot create WARP RGB input pool");
    auto* config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, caps, static_cast<guint>(info.size), 1, 2);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
    auto* params = gst_d3d11_allocation_params_new(wrapped, &info,
        GST_D3D11_ALLOCATION_FLAG_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0);
    require(params != nullptr, "cannot create WARP RGB allocation parameters");
    gst_buffer_pool_config_set_d3d11_allocation_params(config, params);
    gst_d3d11_allocation_params_free(params);
    require(gst_buffer_pool_set_config(pool, config) && gst_buffer_pool_set_active(pool, TRUE),
            "cannot activate WARP RGB input pool");
    GstBuffer* buffer = nullptr;
    require(gst_buffer_pool_acquire_buffer(pool, &buffer, nullptr) == GST_FLOW_OK,
            "cannot acquire WARP RGB input buffer");
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, 60);
    {
        Pipeline pipeline("appsrc name=source format=time ! proresd3d11rgb ! "
                          "appsink name=sink sync=false");
        auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
        require(source != nullptr, "WARP RGB appsrc missing");
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        pipeline.state(GST_STATE_PLAYING);
        require(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK,
                "cannot push WARP RGB input buffer");
        gst_object_unref(source);
        pipeline.expect_error(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
                              "software D3D11 adapter");
        auto* unexpected = gst_app_sink_try_pull_sample(GST_APP_SINK(pipeline.sink),
                                                        200 * GST_MSECOND);
        if (unexpected) gst_sample_unref(unexpected);
        require(unexpected == nullptr, "WARP RGB emitted output");
    }
    require(gst_buffer_pool_set_active(pool, FALSE), "cannot deactivate WARP RGB input pool");
    gst_object_unref(pool);
    gst_caps_unref(caps);
    gst_object_unref(wrapped);
}

static void known_color(GstSample* compressed, bool from_caps) {
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! "
                      "appsink name=sink sync=false");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    auto* caps = gst_caps_copy(gst_sample_get_caps(compressed));
    if (from_caps) gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING, "bt709", nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    if (!from_caps) {
        const guint8 color[] = {1, 1, 1};
        gst_buffer_fill(input, 22, color, 3);
    }
    pipeline.state(GST_STATE_PLAYING);
    require(gst_app_src_push_buffer(GST_APP_SRC(source), input) == GST_FLOW_OK,
            "color input failed");
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    auto* output = pipeline.pull();
    require(output != nullptr, "color output missing");
    check_d3d_sample(output, 0, true);
    gst_sample_unref(output);
    require(pipeline.pull() == nullptr, "extra color frame");
}

static void dynamic_color_caps(GstSample* compressed) {
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! "
                      "appsink name=sink sync=false max-buffers=4");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    require(source != nullptr, "dynamic-color source missing");
    pipeline.state(GST_STATE_PLAYING);
    GstBuffer* retained = nullptr;
    for (guint64 index = 0; index < 3; ++index) {
        const char* color_name = index == 1 ? "bt601" : "bt709";
        GstVideoColorimetry expected{};
        require(gst_video_colorimetry_from_string(&expected, color_name),
                "dynamic-color fixture is invalid");
        auto* caps = gst_caps_copy(gst_sample_get_caps(compressed));
        gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING, color_name, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        gst_caps_unref(caps);

        auto* packet = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
        require(packet != nullptr, "dynamic-color packet copy failed");
        const guint8 unspecified_color[] = {2, 2, 2};
        require(gst_buffer_fill(packet, 22, unspecified_color, sizeof(unspecified_color)) ==
                    sizeof(unspecified_color), "dynamic-color frame tag write failed");
        GST_BUFFER_PTS(packet) = gst_util_uint64_scale(index, GST_SECOND, 60);
        GST_BUFFER_DTS(packet) = GST_BUFFER_PTS(packet);
        GST_BUFFER_DURATION(packet) = gst_util_uint64_scale(1, GST_SECOND, 60);
        require(gst_app_src_push_buffer(GST_APP_SRC(source), packet) == GST_FLOW_OK,
                "dynamic-color input failed");

        auto* output = pipeline.pull();
        require(output != nullptr, "dynamic-color output missing");
        check_d3d_sample(output, gst_util_uint64_scale(index, GST_SECOND, 60),
                         false, 1920, 1080, &expected);
        if (index == 0) retained = gst_buffer_ref(gst_sample_get_buffer(output));
        if (index == 2) {
            GstVideoInfo info{};
            require(gst_video_info_from_caps(&info, gst_sample_get_caps(output)),
                    "dynamic-color video info missing");
            check_same_pixels(info, retained, gst_sample_get_buffer(output),
                              "pixels changed after color-only caps switch");
        }
        gst_sample_unref(output);
    }
    require(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK,
            "dynamic-color EOS failed");
    gst_object_unref(source);
    require(pipeline.pull() == nullptr, "extra dynamic-color frame");
    require(retained != nullptr && gst_buffer_n_memory(retained) == 3,
            "retained D3D11 buffer invalid after color-only caps changes");
    gst_buffer_unref(retained);
}

static void dynamic_rejected_input(GstSample* compressed, bool unsupported_caps) {
    GstSample* retained = nullptr;
    {
        Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! "
                          "appsink name=sink sync=false max-buffers=4");
        auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
        require(source != nullptr, "dynamic-rejection source missing");
        gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(compressed));
        pipeline.state(GST_STATE_PLAYING);
        auto* first = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
        require(first != nullptr &&
                gst_app_src_push_buffer(GST_APP_SRC(source), first) == GST_FLOW_OK,
                "dynamic-rejection first input failed");
        retained = pipeline.pull();
        require(retained != nullptr, "dynamic-rejection first output missing");
        check_d3d_sample(retained, 0);

        if (unsupported_caps) {
            auto* caps = gst_caps_copy(gst_sample_get_caps(compressed));
            gst_caps_set_simple(caps, "variant", G_TYPE_STRING, "unsupported", nullptr);
            gst_app_src_set_caps(GST_APP_SRC(source), caps);
            gst_caps_unref(caps);
        }
        auto* second = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
        require(second != nullptr, "dynamic-rejection second packet copy failed");
        if (!unsupported_caps) {
            const guint8 alpha = 2;
            require(gst_buffer_fill(second, 25, &alpha, 1) == 1,
                    "dynamic-rejection alpha tag write failed");
        }
        GST_BUFFER_PTS(second) = gst_util_uint64_scale(1, GST_SECOND, 60);
        GST_BUFFER_DTS(second) = GST_BUFFER_PTS(second);
        GST_BUFFER_DURATION(second) = gst_util_uint64_scale(1, GST_SECOND, 60);
        const auto flow = gst_app_src_push_buffer(GST_APP_SRC(source), second);
        require(flow == GST_FLOW_OK || flow == GST_FLOW_NOT_NEGOTIATED,
                "dynamic-rejection unexpected push status");
        gst_object_unref(source);
        if (unsupported_caps)
            pipeline.expect_error();
        else
            pipeline.expect_error(GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT);
        auto* unexpected = gst_app_sink_try_pull_sample(GST_APP_SINK(pipeline.sink),
                                                        200 * GST_MSECOND);
        if (unexpected) gst_sample_unref(unexpected);
        require(unexpected == nullptr, "unsupported frame emitted output");
    }
    check_d3d_sample(retained, 0);
    gst_sample_unref(retained);
}

static void reject_unsupported_rgb_color(GstSample* compressed) {
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! "
                      "proresd3d11rgb ! fakesink");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    auto* caps = gst_caps_copy(gst_sample_get_caps(compressed));
    gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING, "bt601", nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    auto* packet = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    const guint8 unspecified_color[] = {2, 2, 2};
    require(gst_buffer_fill(packet, 22, unspecified_color, sizeof(unspecified_color)) ==
                sizeof(unspecified_color), "unsupported-RGB frame tag write failed");
    pipeline.state(GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(source), packet);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    pipeline.expect_error();
}

static void dynamic_caps(GstSample* hd, GstSample* uhd) {
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! "
                      "appsink name=sink sync=false max-buffers=4");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    require(source != nullptr, "dynamic-caps source missing");
    pipeline.state(GST_STATE_PLAYING);
    GstSample* fixtures[] = {hd, uhd, hd};
    for (guint64 index = 0; index < 3; ++index) {
        gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(fixtures[index]));
        auto* packet = gst_buffer_copy_deep(gst_sample_get_buffer(fixtures[index]));
        require(packet != nullptr, "dynamic-caps packet copy failed");
        GST_BUFFER_PTS(packet) = gst_util_uint64_scale(index, GST_SECOND, 60);
        GST_BUFFER_DTS(packet) = GST_BUFFER_PTS(packet);
        GST_BUFFER_DURATION(packet) = gst_util_uint64_scale(1, GST_SECOND, 60);
        require(gst_app_src_push_buffer(GST_APP_SRC(source), packet) == GST_FLOW_OK,
                "dynamic-caps input failed");
    }
    require(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK,
            "dynamic-caps EOS failed");
    gst_object_unref(source);
    GstBuffer* retained_hd = nullptr;
    for (guint64 index = 0; index < 3; ++index) {
        auto* output = pipeline.pull();
        require(output != nullptr, "dynamic-caps output missing");
        const int width = index == 1 ? 3840 : 1920;
        const int height = index == 1 ? 2160 : 1080;
        check_d3d_sample(output, gst_util_uint64_scale(index, GST_SECOND, 60), false,
                         width, height);
        if (index == 0) retained_hd = gst_buffer_ref(gst_sample_get_buffer(output));
        if (index == 2) {
            GstVideoInfo info{};
            require(gst_video_info_from_caps(&info, gst_sample_get_caps(output)),
                    "dynamic-caps HD video info missing");
            check_same_pixels(info, retained_hd, gst_sample_get_buffer(output),
                              "HD pixels changed after dynamic caps switch");
        }
        gst_sample_unref(output);
    }
    require(pipeline.pull() == nullptr, "extra dynamic-caps frame");
    require(retained_hd != nullptr && gst_buffer_n_memory(retained_hd) == 3,
            "retained HD buffer invalid after caps changes");
    for (guint component = 0; component < 3; ++component) {
        D3D11_TEXTURE2D_DESC desc{};
        auto* memory = GST_D3D11_MEMORY_CAST(gst_buffer_peek_memory(retained_hd, component));
        require(gst_d3d11_memory_get_texture_desc(memory, &desc) &&
                desc.Width == (component ? 960u : 1920u) && desc.Height == 1080u,
                "retained HD texture changed after caps renegotiation");
    }
    gst_buffer_unref(retained_hd);
}

static void shared_device_instances(const char* path) {
    auto* pipeline = gst_pipeline_new("shared-device-test");
    auto* device = gst_d3d11_device_new(0, 0);
    require(pipeline && device, "shared D3D11 test setup failed");
    auto* context = gst_d3d11_context_new(device);
    gst_element_set_context(pipeline, context);
    gst_context_unref(context);
    GstElement* sinks[2]{};
    for (int index = 0; index < 2; ++index) {
        auto* source = gst_element_factory_make("filesrc", nullptr);
        auto* demux = gst_element_factory_make("qtdemux", nullptr);
        auto* decoder = gst_element_factory_make("proresd3d11dec", nullptr);
        sinks[index] = gst_element_factory_make("appsink", nullptr);
        require(source && demux && decoder && sinks[index], "shared-device element missing");
        g_object_set(source, "location", path, nullptr);
        g_object_set(sinks[index], "sync", FALSE, "max-buffers", 4u, nullptr);
        gst_bin_add_many(GST_BIN(pipeline), source, demux, decoder, sinks[index], nullptr);
        require(gst_element_link(source, demux) && gst_element_link(decoder, sinks[index]),
                "shared-device static link failed");
        g_signal_connect(demux, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad,
                                                              gpointer target) {
            auto* input = gst_element_get_static_pad(GST_ELEMENT(target), "sink");
            if (!gst_pad_is_linked(input) && gst_pad_link(pad, input) != GST_PAD_LINK_OK)
                GST_ELEMENT_ERROR(GST_ELEMENT(target), CORE, PAD,
                                  ("Shared-device demux link failed"), (nullptr));
            gst_object_unref(input);
        }), decoder);
    }
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "shared-device PLAYING failed");
    for (guint64 frame = 0; frame < 180; ++frame) {
        for (auto* sink : sinks) {
            auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
            require(sample != nullptr, "shared-device output missing");
            check_d3d_sample(sample, gst_util_uint64_scale(frame, GST_SECOND, 60));
            auto* memory = GST_D3D11_MEMORY_CAST(
                gst_buffer_peek_memory(gst_sample_get_buffer(sample), 0));
            require(memory->device == device, "decoder did not honor shared GstD3D11Device");
            gst_sample_unref(sample);
        }
    }
    for (auto* sink : sinks) {
        require(gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND) == nullptr &&
                gst_app_sink_is_eos(GST_APP_SINK(sink)), "shared-device EOS mismatch");
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    require(gst_element_get_state(pipeline, nullptr, nullptr, 10 * GST_SECOND) !=
            GST_STATE_CHANGE_ASYNC, "shared-device NULL timeout");
    gst_object_unref(pipeline);
    gst_object_unref(device);
}

static void interlaced_output(const char* path, GstVideoFieldOrder order, bool alpha,
                              guint expected_frames) {
    Pipeline pipeline(direct_pipeline);
    pipeline.file(path);
    pipeline.state(GST_STATE_PLAYING);
    guint frames = 0;
    while (auto* sample = pipeline.pull()) {
        auto* caps = gst_sample_get_caps(sample);
        auto* buffer = gst_sample_get_buffer(sample);
        GstVideoInfo info{};
        require(GST_BUFFER_PTS(buffer) == gst_util_uint64_scale(frames, GST_SECOND, 30),
                "interlaced sequential PTS mismatch");
        require(gst_video_info_from_caps(&info, caps), "interlaced caps invalid");
        require(info.interlace_mode == GST_VIDEO_INTERLACE_MODE_INTERLEAVED &&
                GST_VIDEO_INFO_FIELD_ORDER(&info) == order,
                "interlaced field order mismatch");
        require(GST_VIDEO_INFO_FORMAT(&info) == (alpha ? GST_VIDEO_FORMAT_AYUV64 :
                                                   GST_VIDEO_FORMAT_I422_10LE),
                "interlaced output format mismatch");
        require(gst_caps_features_contains(gst_caps_get_features(caps, 0),
                                           GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY),
                "interlaced D3D11Memory caps missing");
        require(GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_INTERLACED) &&
                GST_BUFFER_FLAG_IS_SET(buffer, GST_VIDEO_BUFFER_FLAG_TFF) ==
                    (order == GST_VIDEO_FIELD_ORDER_TOP_FIELD_FIRST),
                "interlaced buffer flags mismatch");
        require(gst_buffer_n_memory(buffer) == (alpha ? 1u : 3u),
                "interlaced D3D11 memory count mismatch");
        for (guint i = 0; i < gst_buffer_n_memory(buffer); ++i)
            require(gst_is_d3d11_memory(gst_buffer_peek_memory(buffer, i)),
                    "interlaced output contains CPU memory");
        gst_sample_unref(sample);
        ++frames;
    }
    require(frames == expected_frames, "interlaced EOS count mismatch");
    Pipeline seeking(direct_pipeline);
    seeking.file(path);
    seeking.state(GST_STATE_PLAYING);
    auto* first = seeking.pull();
    require(first != nullptr, "interlaced seek preroll missing");
    gst_sample_unref(first);
    require(gst_element_seek_simple(seeking.pipe, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        gst_util_uint64_scale(15, GST_SECOND, 30)), "interlaced seek rejected");
    for (guint i = 15; i < 18; ++i) {
        auto* sample = seeking.pull();
        require(sample != nullptr, "interlaced seek sample missing");
        require(GST_BUFFER_PTS(gst_sample_get_buffer(sample)) ==
                gst_util_uint64_scale(i, GST_SECOND, 30),
                "interlaced seek PTS mismatch");
        gst_sample_unref(sample);
    }
}

static void reject_corrupt_second_field(GstSample* compressed) {
    auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    guint8 header_size[2]{};
    require(gst_buffer_extract(input, 8, header_size, 2) == 2,
            "field packet has no frame header");
    const auto first = static_cast<gsize>(8 + (header_size[0] << 8) + header_size[1]);
    guint8 picture_size[4]{};
    require(gst_buffer_extract(input, first + 1, picture_size, 4) == 4,
            "field packet has no first picture size");
    const auto second = first +
        (static_cast<gsize>(picture_size[0]) << 24) +
        (static_cast<gsize>(picture_size[1]) << 16) +
        (static_cast<gsize>(picture_size[2]) << 8) + picture_size[3];
    require(second + 8 <= gst_buffer_get_size(input), "field packet has no second picture");
    const guint8 invalid_header = 0;
    require(gst_buffer_fill(input, second, &invalid_header, 1) == 1,
            "cannot corrupt second picture header");
    Pipeline pipeline("appsrc name=source format=time ! proresd3d11dec ! fakesink");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(compressed));
    pipeline.state(GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(source), input);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    pipeline.expect_error(GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT);
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    require(argc >= 2 && (argc <= 4 || argc == 8),
            "d3d11_plugin_smoke hq.mov [4k.mov] [alpha.mov] [tff.mov bff.mov alpha-tff.mov alpha-bff.mov]");
    const auto capabilities = check_device_capabilities();
    GstBuffer* retained = nullptr;
    {
        Pipeline pipeline(direct_pipeline);
        pipeline.file(argv[1]);
        for (int cycle = 0; cycle < 3; ++cycle) {
            pipeline.state(GST_STATE_PAUSED);
            auto* preroll = gst_app_sink_try_pull_preroll(GST_APP_SINK(pipeline.sink),
                                                          10 * GST_SECOND);
            pipeline.errors();
            require(preroll != nullptr, "preroll missing");
            check_d3d_sample(preroll, 0);
            gst_sample_unref(preroll);
            pipeline.state(GST_STATE_PLAYING);
            guint64 count = 0;
            while (auto* sample = pipeline.pull()) {
                check_d3d_sample(sample, gst_util_uint64_scale(count, GST_SECOND, 60));
                if (cycle == 0 && count == 0)
                    retained = gst_buffer_ref(gst_sample_get_buffer(sample));
                gst_sample_unref(sample);
                ++count;
            }
            require(count == 180, "EOS frame count mismatch");
            pipeline.state(GST_STATE_NULL);
            require(gst_element_get_state(pipeline.pipe, nullptr, nullptr, 5 * GST_SECOND) !=
                    GST_STATE_CHANGE_ASYNC, "NULL timeout");
            require(retained && gst_buffer_n_memory(retained) == 3,
                    "retained D3D11 buffer invalid after stop");
        }
        pipeline.state(GST_STATE_PLAYING);
        auto* first = pipeline.pull();
        require(first != nullptr, "restart missing frame");
        gst_sample_unref(first);
        for (guint64 index : {60ULL, 30ULL, 120ULL, 0ULL}) {
            const auto target = gst_util_uint64_scale(index, GST_SECOND, 60);
            require(gst_element_seek_simple(pipeline.pipe, GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), target),
                "seek rejected");
            for (int offset = 0; offset < 3; ++offset) {
                auto* sample = pipeline.pull();
                require(sample != nullptr, "seek output missing");
                check_d3d_sample(sample,
                    gst_util_uint64_scale(index + offset, GST_SECOND, 60));
                gst_sample_unref(sample);
            }
        }
    }
    require(retained && gst_buffer_n_memory(retained) == 3,
            "buffer invalid after pipeline destruction");
    gst_buffer_unref(retained);
    shared_device_instances(argv[1]);

    {
        Pipeline pipeline("filesrc name=source ! qtdemux name=demux "
                          "proresd3d11dec name=decoder ! d3d11download ! "
                          "video/x-raw,format=I422_10LE ! "
                          "appsink name=sink sync=false max-buffers=1");
        pipeline.file(argv[1]);
        pipeline.state(GST_STATE_PLAYING);
        auto* sample = pipeline.pull();
        require(sample != nullptr, "download validation frame missing");
        check_downloaded_sample(sample);
        gst_sample_unref(sample);
    }

    GstSample* compressed = nullptr;
    {
        Pipeline demux("filesrc name=source ! qtdemux ! appsink name=sink sync=false max-buffers=1");
        demux.file(argv[1]);
        demux.state(GST_STATE_PLAYING);
        compressed = demux.pull();
        require(compressed != nullptr, "compressed fixture missing");
    }
    known_color(compressed, false);
    known_color(compressed, true);
    dynamic_color_caps(compressed);
    reject_software_device(compressed);
    dynamic_rejected_input(compressed, false);
    dynamic_rejected_input(compressed, true);
    reject_unsupported_rgb_color(compressed);
    if (argc >= 3) {
        Pipeline demux("filesrc name=source ! qtdemux ! appsink name=sink sync=false max-buffers=1");
        demux.file(argv[2]);
        demux.state(GST_STATE_PLAYING);
        auto* uhd = demux.pull();
        require(uhd != nullptr, "4K compressed fixture missing");
        dynamic_caps(compressed, uhd);
        gst_sample_unref(uhd);
    }
    for (const char* test : {"bad-signature", "alpha-hidden-in-caps",
                             "invalid-alpha-mode",
                             "interlaced-hidden-in-caps", "invalid-chroma-flags",
                             "oversized-first-dc", "ac-run-boundary"})
        injected_error(compressed, test);
    delayed_entropy_error(compressed);
    gst_sample_unref(compressed);

    if (argc >= 4) {
        Pipeline demux("filesrc name=source ! qtdemux ! appsink name=sink sync=false max-buffers=1");
        demux.file(argv[3]);
        demux.state(GST_STATE_PLAYING);
        auto* alpha = demux.pull();
        require(alpha != nullptr, "alpha corruption fixture missing");
        alpha_entropy_error(alpha);
        gst_sample_unref(alpha);
    }
    if (argc == 8) {
        interlaced_output(argv[4], GST_VIDEO_FIELD_ORDER_TOP_FIELD_FIRST, false, 30);
        interlaced_output(argv[5], GST_VIDEO_FIELD_ORDER_BOTTOM_FIELD_FIRST, false, 30);
        interlaced_output(argv[6], GST_VIDEO_FIELD_ORDER_TOP_FIELD_FIRST, true, 30);
        interlaced_output(argv[7], GST_VIDEO_FIELD_ORDER_BOTTOM_FIELD_FIRST, true, 30);
        Pipeline demux("filesrc name=source ! qtdemux ! appsink name=sink sync=false max-buffers=1");
        demux.file(argv[4]);
        demux.state(GST_STATE_PLAYING);
        auto* compressed_field = demux.pull();
        require(compressed_field != nullptr, "interlaced corruption fixture missing");
        reject_corrupt_second_field(compressed_field);
        gst_sample_unref(compressed_field);
    }

    {
        Pipeline pipeline(direct_pipeline);
        pipeline.file(argv[1]);
        auto* decoder = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "decoder");
        g_object_set(decoder, "adapter", 31, nullptr);
        gst_object_unref(decoder);
        gst_element_set_state(pipeline.pipe, GST_STATE_PLAYING);
        pipeline.expect_error();
    }
    {
        Pipeline pipeline(direct_pipeline);
        pipeline.file("media/does-not-exist-d3d11-plugin-smoke.mov");
        gst_element_set_state(pipeline.pipe, GST_STATE_PLAYING);
        pipeline.expect_error();
    }

    std::cout << "{\"passed\":true,\"eos_cycles\":3,\"frames_per_cycle\":180,"
                 "\"flushing_seeks\":4,\"known_color_cases\":2,"
                 "\"dynamic_color_caps_changes\":2,"
                 "\"dynamic_rejected_input_cases\":2,"
                 "\"dynamic_caps_changes\":" << (argc >= 3 ? 2 : 0) << ","
                 "\"retained_buffer_after_destroy\":true,\"shared_device_instances\":2,"
                 "\"feature_level\":" << capabilities.feature_level << ","
                 "\"r16_format_support\":" << capabilities.r16_support << ","
                 "\"software_adapter_decoder_rejected\":true,"
                 "\"software_adapter_rgb_rejected\":true,\"error_cases\":"
              << (argc == 8 ? 17 : argc >= 4 ? 16 : 15) << ",\"interlaced_cases\":"
              << (argc == 8 ? 4 : 0) << ","
                 "\"output\":\"I422_10LE D3D11Memory (three R16_UNORM UAV textures)\"}\n";
    gst_deinit();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
