// 純粋DX11 GStreamer要素のD3D11Memory、時刻、seek、寿命、失敗入力を検証する。
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/d3d11/gstd3d11utils.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <d3d11.h>

#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
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

    void expect_error() {
        auto* message = gst_bus_timed_pop_filtered(bus, 15 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        require(message != nullptr, "expected error timed out");
        const bool is_error = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
        if (is_error) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            std::cerr << "expected_error=" << error->message << '\n';
            g_error_free(error);
            g_free(debug);
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

static void check_d3d_sample(GstSample* sample, GstClockTime pts, bool full_bt709 = false) {
    auto* buffer = gst_sample_get_buffer(sample);
    require(GST_BUFFER_PTS(buffer) == pts, "PTS mismatch");
    require(GST_BUFFER_DURATION(buffer) > 0 &&
            GST_BUFFER_DURATION(buffer) != GST_CLOCK_TIME_NONE, "duration missing");
    check_caps(sample, full_bt709);
    auto* caps = gst_sample_get_caps(sample);
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
        require(desc.Width == static_cast<UINT>(component ? 960 : 1920) && desc.Height == 1080,
                "unexpected output texture size");
    }
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
    if (std::string(test) == "4444-caps")
        gst_caps_set_simple(caps, "variant", G_TYPE_STRING, "4444", nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    guint8 byte = 0;
    if (std::string(test) == "bad-signature") gst_buffer_fill(input, 4, &byte, 1);
    if (std::string(test) == "alpha-hidden-in-caps") {
        byte = 2;
        gst_buffer_fill(input, 25, &byte, 1);
    }
    if (std::string(test) == "interlaced-hidden-in-caps") {
        gst_buffer_extract(input, 20, &byte, 1);
        byte |= 4;
        gst_buffer_fill(input, 20, &byte, 1);
    }
    pipeline.state(GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(source), input);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    pipeline.expect_error();
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

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    require(argc == 2, "d3d11_plugin_smoke 180-frame-60fps-hq.mov");
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
    for (const char* test : {"bad-signature", "alpha-hidden-in-caps",
                             "interlaced-hidden-in-caps", "4444-caps"})
        injected_error(compressed, test);
    gst_sample_unref(compressed);

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
                 "\"retained_buffer_after_destroy\":true,\"shared_device_instances\":2,"
                 "\"error_cases\":6,"
                 "\"output\":\"I422_10LE D3D11Memory (three R16_UNORM UAV textures)\"}\n";
    gst_deinit();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
