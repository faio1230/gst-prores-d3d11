// 実際のGStreamerパイプラインで時刻、EOS、seek、寿命、失敗入力を検証する。
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <fstream>
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
        // gst_parse_launchの一度限りの遅延リンクではNULLからの再起動を検査できない。
        if (auto* demux = gst_bin_get_by_name(GST_BIN(pipe), "demux")) {
            g_signal_connect(demux, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer data) {
                auto* target = gst_bin_get_by_name(GST_BIN(data), "decoder");
                auto* input = gst_element_get_static_pad(target, "sink");
                if (!gst_pad_is_linked(input) && gst_pad_link(pad, input) != GST_PAD_LINK_OK)
                    GST_ELEMENT_ERROR(target, CORE, PAD, ("Test demux link failed"), (nullptr));
                gst_object_unref(input); gst_object_unref(target);
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
        require(gst_element_set_state(pipe, target) != GST_STATE_CHANGE_FAILURE, "state transition failed");
    }
    void errors() {
        auto* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (!msg) return;
        GError* error = nullptr; gchar* debug = nullptr;
        gst_message_parse_error(msg, &error, &debug);
        std::string text = error ? error->message : "unknown bus error";
        if (debug) text += std::string(" : ") + debug;
        g_clear_error(&error); g_free(debug); gst_message_unref(msg);
        throw std::runtime_error(text);
    }
    GstSample* pull() {
        auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        errors();
        if (!sample) require(gst_app_sink_is_eos(GST_APP_SINK(sink)), "sample timeout");
        return sample;
    }
    void expect_error() {
        auto* msg = gst_bus_timed_pop_filtered(bus, 15 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        require(msg != nullptr, "expected error timed out");
        bool error = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
        if (error) { GError* e = nullptr; gchar* debug = nullptr; gst_message_parse_error(msg, &e, &debug); std::cerr << "expected_error=" << e->message << '\n'; g_error_free(e); g_free(debug); }
        gst_message_unref(msg);
        require(error, "unexpected successful EOS");
    }
};

static constexpr auto decode_pipeline =
    "filesrc name=source ! qtdemux name=demux proresvkdec name=decoder ! "
    "appsink name=sink sync=false max-buffers=4";

static void check_sample(GstSample* sample, GstClockTime pts, std::ofstream* raw = nullptr, bool full_bt709 = false) {
    auto* buffer = gst_sample_get_buffer(sample);
    require(GST_BUFFER_PTS(buffer) == pts, "PTS mismatch");
    require(GST_BUFFER_DURATION(buffer) > 0 && GST_BUFFER_DURATION(buffer) != GST_CLOCK_TIME_NONE, "duration missing");
    GstVideoInfo info;
    require(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)), "invalid video caps");
    require(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_I422_10LE, "unexpected pixel format");
    require(info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235 && info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709 &&
            info.colorimetry.primaries == (full_bt709 ? GST_VIDEO_COLOR_PRIMARIES_BT709 : GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) &&
            info.colorimetry.transfer == (full_bt709 ? GST_VIDEO_TRANSFER_BT709 : GST_VIDEO_TRANSFER_UNKNOWN), "color metadata mismatch");
    GstVideoFrame frame{};
    require(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ), "output mapping failed");
    bool nonzero = false;
    for (guint p = 0; p < 3; ++p) {
        const int w = p ? info.width / 2 : info.width;
        for (int y = 0; y < info.height; ++y) {
            auto* data = static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, p)) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, p);
            if (!p) for (int x = 0; x < w; ++x) if (reinterpret_cast<guint16*>(data)[x]) { nonzero = true; break; }
            if (raw) raw->write(reinterpret_cast<char*>(data), w * 2);
        }
    }
    gst_video_frame_unmap(&frame);
    require(nonzero, "all-zero luma plane in nonblack test pattern");
    if (raw) require(static_cast<bool>(*raw), "raw dump write failed");
}

static std::string digest(GstBuffer* buffer) {
    GstMapInfo map{};
    require(gst_buffer_map(buffer, &map, GST_MAP_READ), "retained buffer map failed");
    auto* value = g_compute_checksum_for_data(G_CHECKSUM_SHA256, map.data, map.size);
    std::string result = value;
    g_free(value); gst_buffer_unmap(buffer, &map);
    return result;
}

static void injected_error(GstSample* compressed, const char* test) {
    Pipeline pipeline("appsrc name=source format=time ! proresvkdec ! fakesink");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    auto* caps = gst_caps_copy(gst_sample_get_caps(compressed));
    if (std::string(test) == "4444-caps") gst_caps_set_simple(caps, "variant", G_TYPE_STRING, "4444", nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps); gst_caps_unref(caps);
    auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    guint8 byte = 0;
    if (std::string(test) == "bad-signature") gst_buffer_fill(input, 4, &byte, 1);
    if (std::string(test) == "alpha-hidden-in-caps") { byte = 2; gst_buffer_fill(input, 25, &byte, 1); }
    if (std::string(test) == "interlaced-hidden-in-caps") { gst_buffer_extract(input, 20, &byte, 1); byte |= 4; gst_buffer_fill(input, 20, &byte, 1); }
    pipeline.state(GST_STATE_PLAYING);
    gst_app_src_push_buffer(GST_APP_SRC(source), input);
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
    pipeline.expect_error();
}

static void known_color(GstSample* compressed, bool from_caps) {
    Pipeline pipeline("appsrc name=source format=time ! proresvkdec ! appsink name=sink sync=false");
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "source");
    auto* caps = gst_caps_copy(gst_sample_get_caps(compressed));
    if (from_caps) gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING, "bt709", nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps); gst_caps_unref(caps);
    auto* input = gst_buffer_copy_deep(gst_sample_get_buffer(compressed));
    if (!from_caps) { const guint8 color[] = {1, 1, 1}; gst_buffer_fill(input, 22, color, 3); }
    pipeline.state(GST_STATE_PLAYING);
    require(gst_app_src_push_buffer(GST_APP_SRC(source), input) == GST_FLOW_OK, "color input failed");
    gst_app_src_end_of_stream(GST_APP_SRC(source)); gst_object_unref(source);
    auto* output = pipeline.pull(); require(output != nullptr, "color output missing");
    check_sample(output, 0, nullptr, true); gst_sample_unref(output);
    require(pipeline.pull() == nullptr, "extra color frame");
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    require(argc == 3, "plugin_smoke 180-frame-60fps-hq.mov first30.raw");
    std::ofstream raw(argv[2], std::ios::binary); require(static_cast<bool>(raw), "cannot open raw file");
    GstBuffer* retained = nullptr;
    std::string retained_hash;
    {
        Pipeline pipeline(decode_pipeline);
        pipeline.file(argv[1]);
        for (int cycle = 0; cycle < 3; ++cycle) {
            pipeline.state(GST_STATE_PAUSED);
            auto* preroll = gst_app_sink_try_pull_preroll(GST_APP_SINK(pipeline.sink), 10 * GST_SECOND);
            pipeline.errors(); require(preroll != nullptr, "preroll missing");
            check_sample(preroll, 0); gst_sample_unref(preroll);
            pipeline.state(GST_STATE_PLAYING);
            guint64 count = 0;
            while (auto* sample = pipeline.pull()) {
                check_sample(sample, gst_util_uint64_scale(count, GST_SECOND, 60), cycle == 0 && count < 30 ? &raw : nullptr);
                if (cycle == 0 && count == 0) { retained = gst_buffer_ref(gst_sample_get_buffer(sample)); retained_hash = digest(retained); }
                gst_sample_unref(sample); ++count;
            }
            require(count == 180, "EOS frame count mismatch");
            pipeline.state(GST_STATE_NULL);
            require(gst_element_get_state(pipeline.pipe, nullptr, nullptr, 5 * GST_SECOND) != GST_STATE_CHANGE_ASYNC, "NULL timeout");
            require(digest(retained) == retained_hash, "retained buffer changed after stop");
        }
        pipeline.state(GST_STATE_PLAYING);
        auto* first = pipeline.pull(); require(first != nullptr, "restart missing frame"); gst_sample_unref(first);
        for (guint64 index : {60ULL, 30ULL, 120ULL, 0ULL}) {
            const auto target = gst_util_uint64_scale(index, GST_SECOND, 60);
            require(gst_element_seek_simple(pipeline.pipe, GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), target), "seek rejected");
            for (int n = 0; n < 3; ++n) {
                auto* sample = pipeline.pull(); require(sample != nullptr, "seek output missing");
                check_sample(sample, gst_util_uint64_scale(index + n, GST_SECOND, 60)); gst_sample_unref(sample);
            }
        }
    }
    require(digest(retained) == retained_hash, "buffer invalid after pipeline destruction");
    gst_buffer_unref(retained);
    GstSample* compressed = nullptr;
    {
        Pipeline demux("filesrc name=source ! qtdemux ! appsink name=sink sync=false max-buffers=1");
        demux.file(argv[1]); demux.state(GST_STATE_PLAYING); compressed = demux.pull();
        require(compressed != nullptr, "compressed fixture missing");
    }
    known_color(compressed, false);
    known_color(compressed, true);
    for (const char* test : {"bad-signature", "alpha-hidden-in-caps", "interlaced-hidden-in-caps", "4444-caps"}) injected_error(compressed, test);
    gst_sample_unref(compressed);
    {
        Pipeline pipeline(decode_pipeline); pipeline.file(argv[1]);
        auto* decoder = gst_bin_get_by_name(GST_BIN(pipeline.pipe), "decoder");
        g_object_set(decoder, "device-index", 31u, nullptr); gst_object_unref(decoder);
        pipeline.state(GST_STATE_PLAYING); pipeline.expect_error();
    }
    {
        Pipeline pipeline(decode_pipeline); pipeline.file("media/does-not-exist-plugin-smoke.mov");
        gst_element_set_state(pipeline.pipe, GST_STATE_PLAYING); pipeline.expect_error();
    }
    std::cout << "{\"passed\":true,\"eos_cycles\":3,\"frames_per_cycle\":180,\"flushing_seeks\":4,\"known_color_cases\":2,\"retained_buffer_after_destroy\":true,\"error_cases\":6,\"output\":\"I422_10LE system memory\"}\n";
    gst_deinit();
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
