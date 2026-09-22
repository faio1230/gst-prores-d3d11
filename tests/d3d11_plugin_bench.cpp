// GStreamerのCPU経路と純粋DX11 pluginを同じ素材・I422_10LE条件で測る。
#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static double cpu_seconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user),
            "GetProcessTimes failed");
    ULARGE_INTEGER kernel_value{}, user_value{};
    kernel_value.LowPart = kernel.dwLowDateTime;
    kernel_value.HighPart = kernel.dwHighDateTime;
    user_value.LowPart = user.dwLowDateTime;
    user_value.HighPart = user.dwHighDateTime;
    return (kernel_value.QuadPart + user_value.QuadPart) * 1e-7;
}

static double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(std::ceil(fraction * (values.size() - 1)))];
}

struct Pipeline {
    GstElement* pipe = nullptr;
    GstElement* sink = nullptr;
    GstBus* bus = nullptr;

    Pipeline(const std::string& description, const char* input) {
        GError* error = nullptr;
        pipe = gst_parse_launch(description.c_str(), &error);
        if (error) {
            std::string text = error->message;
            g_error_free(error);
            if (pipe) gst_object_unref(pipe);
            throw std::runtime_error(text);
        }
        require(pipe != nullptr, "pipeline missing");
        sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
        bus = gst_element_get_bus(pipe);
        require(sink && bus, "pipeline endpoints missing");
        auto* source = gst_bin_get_by_name(GST_BIN(pipe), "source");
        require(source != nullptr, "source missing");
        g_object_set(source, "location", input, nullptr);
        gst_object_unref(source);
        auto* demux = gst_bin_get_by_name(GST_BIN(pipe), "demux");
        require(demux != nullptr, "demux missing");
        g_signal_connect(demux, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad,
                                                              gpointer data) {
            auto* decoder = gst_bin_get_by_name(GST_BIN(data), "decoder");
            auto* input_pad = gst_element_get_static_pad(decoder, "sink");
            if (!gst_pad_is_linked(input_pad) &&
                gst_pad_link(pad, input_pad) != GST_PAD_LINK_OK)
                GST_ELEMENT_ERROR(decoder, CORE, PAD, ("Benchmark demux link failed"), (nullptr));
            gst_object_unref(input_pad);
            gst_object_unref(decoder);
        }), pipe);
        gst_object_unref(demux);
    }

    ~Pipeline() {
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(bus);
        gst_object_unref(pipe);
    }

    void play() {
        require(gst_element_set_state(pipe, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
                "PLAYING transition failed");
    }

    void throw_bus_error() {
        auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (!message) return;
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::string text = error ? error->message : "unknown GStreamer error";
        if (debug) text += std::string(" : ") + debug;
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        throw std::runtime_error(text);
    }

    GstSample* pull() {
        auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 30 * GST_SECOND);
        throw_bus_error();
        require(sample != nullptr, "sample timeout or unexpected EOS");
        return sample;
    }

    void seek(GstClockTime target) {
        require(gst_element_seek_simple(pipe, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), target),
            "flushing seek rejected");
    }
};

static std::string pipeline_description(const std::string& mode) {
    const std::string prefix = "filesrc name=source ! qtdemux name=demux ";
    const std::string suffix = " ! appsink name=sink sync=false max-buffers=8";
    if (mode == "dx11-direct")
        return prefix + "proresd3d11dec name=decoder" + suffix;
    if (mode == "dx11-download")
        return prefix + "proresd3d11dec name=decoder ! d3d11download ! "
                        "video/x-raw,format=I422_10LE" + suffix;
    if (mode == "cpu")
        return prefix + "avdec_prores name=decoder ! video/x-raw,format=I422_10LE" + suffix;
    if (mode == "cpu-slice")
        return prefix + "avdec_prores name=decoder max-threads=0 thread-type=slice ! "
                        "video/x-raw,format=I422_10LE" + suffix;
    throw std::runtime_error("mode must be dx11-direct, dx11-download, cpu, or cpu-slice");
}

struct StreamContract {
    int width = 0;
    int height = 0;
    int fps_n = 0;
    int fps_d = 1;
};

static StreamContract verify_sample(GstSample* sample, const std::string& mode,
                                    std::uint64_t frame_index) {
    auto* caps = gst_sample_get_caps(sample);
    GstVideoInfo info{};
    require(gst_video_info_from_caps(&info, caps), "invalid output caps");
    require(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_I422_10LE,
            "unexpected output format");
    const bool d3d = gst_caps_features_contains(gst_caps_get_features(caps, 0),
                                                GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY);
    require(d3d == (mode == "dx11-direct"), "unexpected output memory feature");
    auto* buffer = gst_sample_get_buffer(sample);
    if (d3d) {
        require(gst_buffer_n_memory(buffer) == 3, "D3D11 output plane count mismatch");
        for (guint plane = 0; plane < 3; ++plane)
            require(gst_is_d3d11_memory(gst_buffer_peek_memory(buffer, plane)),
                    "non-D3D11 memory in direct mode");
    }
    if (info.fps_n > 0) {
        const auto expected = gst_util_uint64_scale(frame_index * info.fps_d,
                                                    GST_SECOND, info.fps_n);
        require(GST_BUFFER_PTS(buffer) == expected, "output PTS mismatch");
    }
    require(GST_BUFFER_DURATION(buffer) != GST_CLOCK_TIME_NONE &&
            GST_BUFFER_DURATION(buffer) > 0, "output duration missing");
    return {info.width, info.height, info.fps_n, info.fps_d};
}

static double run_startup(const std::string& description, const char* input,
                          const std::string& mode) {
    const auto begin = Clock::now();
    Pipeline pipeline(description, input);
    pipeline.play();
    auto* sample = pipeline.pull();
    verify_sample(sample, mode, 0);
    gst_sample_unref(sample);
    return milliseconds(begin, Clock::now());
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    if (argc < 5 || argc > 10)
        throw std::runtime_error("d3d11_plugin_bench input mode frames.csv loops "
                                 "[warmup=30] [frames-per-loop=180] [seeks=4] [startups=3] "
                                 "[minimum-seconds=0]");
    const std::string mode = argv[2];
    const int loops = std::stoi(argv[4]);
    const int warmup = argc > 5 ? std::stoi(argv[5]) : 30;
    const int frames_per_loop = argc > 6 ? std::stoi(argv[6]) : 180;
    const int seek_count = argc > 7 ? std::stoi(argv[7]) : 4;
    const int startup_count = argc > 8 ? std::stoi(argv[8]) : 3;
    const int minimum_seconds = argc > 9 ? std::stoi(argv[9]) : 0;
    require(loops > 0 && warmup >= 0 && frames_per_loop > warmup &&
            seek_count >= 0 && startup_count >= 0 && minimum_seconds >= 0,
            "invalid numeric argument");
    const auto description = pipeline_description(mode);
    std::ofstream csv(argv[3]);
    require(static_cast<bool>(csv), "cannot open output CSV");
    csv << "epoch,frame,pts_ns,delivery_interval_ms,steady\n";

    const auto process_begin = Clock::now();
    const double cpu_begin = cpu_seconds();
    const auto create_begin = Clock::now();
    Pipeline pipeline(description, argv[1]);
    const auto create_end = Clock::now();
    pipeline.play();
    const auto play_end = Clock::now();
    auto previous = play_end;
    double first_buffer_ms = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t steady_frames = 0;
    double steady_elapsed_ms = 0;
    std::vector<double> intervals;
    std::vector<double> seek_times;
    StreamContract contract{};

    int completed_epochs = 0;
    for (int epoch = 0; epoch < loops ||
         milliseconds(process_begin, Clock::now()) < minimum_seconds * 1000.0; ++epoch) {
        Clock::time_point seek_begin{};
        if (epoch) {
            seek_begin = Clock::now();
            pipeline.seek(0);
        }
        for (int frame = 0; frame < frames_per_loop; ++frame) {
            auto* sample = pipeline.pull();
            const auto delivered = Clock::now();
            contract = verify_sample(sample, mode, static_cast<std::uint64_t>(frame));
            const double interval = milliseconds(previous, delivered);
            previous = delivered;
            if (!total_frames) first_buffer_ms = milliseconds(process_begin, delivered);
            if (epoch && !frame) seek_times.push_back(milliseconds(seek_begin, delivered));
            const bool steady = frame >= warmup;
            if (steady) {
                ++steady_frames;
                steady_elapsed_ms += interval;
                intervals.push_back(interval);
            }
            csv << epoch << ',' << frame << ',' << GST_BUFFER_PTS(gst_sample_get_buffer(sample))
                << ',' << interval << ',' << (steady ? 1 : 0) << '\n';
            gst_sample_unref(sample);
            ++total_frames;
        }
        ++completed_epochs;
    }

    for (int iteration = 0; iteration < seek_count; ++iteration) {
        const auto frame = static_cast<std::uint64_t>((iteration * 73) % (frames_per_loop - 2));
        const auto target = gst_util_uint64_scale(frame * contract.fps_d,
                                                  GST_SECOND, contract.fps_n);
        const auto begin = Clock::now();
        pipeline.seek(target);
        auto* sample = pipeline.pull();
        verify_sample(sample, mode, frame);
        seek_times.push_back(milliseconds(begin, Clock::now()));
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline.pipe, GST_STATE_NULL);
    require(gst_element_get_state(pipeline.pipe, nullptr, nullptr, 10 * GST_SECOND) !=
            GST_STATE_CHANGE_ASYNC, "NULL transition timeout");

    std::vector<double> startup_times;
    startup_times.reserve(static_cast<std::size_t>(startup_count));
    for (int iteration = 0; iteration < startup_count; ++iteration)
        startup_times.push_back(run_startup(description, argv[1], mode));

    const auto process_end = Clock::now();
    const double wall_ms = milliseconds(process_begin, process_end);
    const double cpu_used = cpu_seconds() - cpu_begin;
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    require(GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)),
        "GetProcessMemoryInfo failed");
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const char* gpu_completion_wait = mode == "dx11-download" ? "true" :
                                      mode == "dx11-direct" ? "false" : "null";
    std::cout << "{\"passed\":true,\"mode\":\"" << mode << "\""
              << ",\"width\":" << contract.width << ",\"height\":" << contract.height
              << ",\"requested_loops\":" << loops
              << ",\"completed_loops\":" << completed_epochs
              << ",\"minimum_seconds\":" << minimum_seconds
              << ",\"frames\":" << total_frames
              << ",\"warmup_per_loop\":" << warmup
              << ",\"gpu_completion_wait\":" << gpu_completion_wait
              << ",\"pipeline_create_ms\":" << milliseconds(create_begin, create_end)
              << ",\"state_change_submit_ms\":" << milliseconds(create_end, play_end)
              << ",\"first_buffer_ms\":" << first_buffer_ms
              << ",\"steady_frames\":" << steady_frames
              << ",\"steady_fps\":"
              << (steady_elapsed_ms ? steady_frames * 1000.0 / steady_elapsed_ms : 0)
              << ",\"interval_p50_ms\":" << percentile(intervals, 0.50)
              << ",\"interval_p95_ms\":" << percentile(intervals, 0.95)
              << ",\"interval_p99_ms\":" << percentile(intervals, 0.99)
              << ",\"seek_count\":" << seek_times.size()
              << ",\"seek_first_p95_ms\":" << percentile(seek_times, 0.95)
              << ",\"startup_count\":" << startup_times.size()
              << ",\"startup_p95_ms\":" << percentile(startup_times, 0.95)
              << ",\"wall_ms\":" << wall_ms
              << ",\"cpu_seconds\":" << cpu_used
              << ",\"cpu_machine_percent\":"
              << (wall_ms ? cpu_used * 100000.0 /
                  (wall_ms * system.dwNumberOfProcessors) : 0)
              << ",\"peak_working_set_mib\":" << memory.PeakWorkingSetSize / 1048576.0
              << ",\"private_mib_end\":" << memory.PrivateUsage / 1048576.0 << "}\n";
    gst_deinit();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
