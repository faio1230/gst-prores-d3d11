// 専用RGB要素のD3D11Memory、EOS、seek、停止後のbuffer寿命を確認する。
#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <d3d11.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

static void check_sample(GstSample* sample, GstVideoInfo* info) {
    require(sample != nullptr, "RGB sample timeout");
    auto* caps = gst_sample_get_caps(sample);
    const auto format = gst_video_info_from_caps(info, caps) ?
        GST_VIDEO_INFO_FORMAT(info) : GST_VIDEO_FORMAT_UNKNOWN;
    require((format == GST_VIDEO_FORMAT_RGB10A2_LE ||
             format == GST_VIDEO_FORMAT_RGBA64_LE) &&
            info->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 &&
            info->colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_RGB &&
            info->colorimetry.transfer == GST_VIDEO_TRANSFER_BT709 &&
            info->colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_BT709 &&
            gst_caps_features_contains(gst_caps_get_features(caps, 0),
                                       GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY),
            "RGB output caps are not full BT.709 D3D11Memory");
    auto* buffer = gst_sample_get_buffer(sample);
    require(gst_buffer_n_memory(buffer) == 1, "RGB output must have one D3D11Memory");
    auto* memory = gst_buffer_peek_memory(buffer, 0);
    require(gst_is_d3d11_memory(memory), "RGB output is not D3D11Memory");
    D3D11_TEXTURE2D_DESC desc{};
    require(gst_d3d11_memory_get_texture_desc(GST_D3D11_MEMORY_CAST(memory), &desc) &&
            desc.Format == (format == GST_VIDEO_FORMAT_RGBA64_LE ?
                            DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM) &&
            desc.Width == static_cast<UINT>(info->width) &&
            desc.Height == static_cast<UINT>(info->height) &&
            (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS),
            "RGB output texture has unexpected typed UAV format");
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    require(argc == 3, "usage: d3d11_rgb_element_smoke input.mov expected_frames");
    const int expected_frames = std::stoi(argv[2]);
    require(expected_frames > 0, "expected_frames must be positive");
    GError* error = nullptr;
    auto* pipeline = gst_parse_launch(
        "filesrc name=source ! qtdemux ! proresd3d11dec ! proresd3d11rgb ! "
        "appsink name=sink sync=false max-buffers=2", &error);
    if (error || !pipeline) {
        const std::string message = error ? error->message : "pipeline construction failed";
        if (error) g_error_free(error);
        throw std::runtime_error(message);
    }
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    require(source && sink, "missing pipeline endpoint");
    g_object_set(source, "location", argv[1], nullptr);
    gst_object_unref(source);
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "pipeline could not play");
    GstBuffer* retained = nullptr;
    GstVideoInfo info{};
    int frames = 0;
    GstClockTime previous_pts = GST_CLOCK_TIME_NONE;
    while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND)) {
        check_sample(sample, &info);
        auto* buffer = gst_sample_get_buffer(sample);
        const auto pts = GST_BUFFER_PTS(buffer);
        require(GST_CLOCK_TIME_IS_VALID(pts) &&
                (!GST_CLOCK_TIME_IS_VALID(previous_pts) || pts > previous_pts),
                "RGB output PTS missing or not increasing");
        previous_pts = pts;
        if (!retained) retained = gst_buffer_ref(buffer);
        ++frames;
        gst_sample_unref(sample);
    }
    require(gst_app_sink_is_eos(GST_APP_SINK(sink)), "RGB pipeline ended before EOS");
    if (frames != expected_frames)
        throw std::runtime_error("RGB frame count mismatch: " + std::to_string(frames) +
                                 "/" + std::to_string(expected_frames));
    require(gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0),
            "RGB seek failed");
    auto* after_seek = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND);
    check_sample(after_seek, &info);
    require(GST_BUFFER_PTS(gst_sample_get_buffer(after_seek)) == 0,
            "RGB seek did not return to PTS zero");
    gst_sample_unref(after_seek);
    require(gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE,
            "RGB pipeline could not stop");
    require(gst_element_get_state(pipeline, nullptr, nullptr, 10 * GST_SECOND) !=
            GST_STATE_CHANGE_FAILURE, "RGB pipeline stop failed");
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    require(retained && gst_buffer_n_memory(retained) == 1,
            "retained RGB buffer was invalidated by pipeline destroy");
    GstVideoFrame frame{};
    require(gst_video_frame_map(&frame, &info, retained, GST_MAP_READ),
            "retained RGB buffer could not be read back after pipeline destroy");
    const auto* row = static_cast<const std::uint32_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const auto first = row[0];
    gst_video_frame_unmap(&frame);
    gst_buffer_unref(retained);
    std::cout << "{\"passed\":true,\"frames\":" << frames
              << ",\"seek\":true,\"retained_rgb_after_destroy\":true,\"first_pixel\":"
              << first << "}\n";
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "d3d11_rgb_element_smoke: " << exception.what() << '\n';
    return 1;
}
