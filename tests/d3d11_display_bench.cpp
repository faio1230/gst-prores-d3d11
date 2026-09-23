// DX11 decode→RGB10A2→実際のD3D11 swapchain sinkを時計同期で計測する。
#include <gst/gst.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point first, Clock::time_point second) {
    return std::chrono::duration<double, std::milli>(second - first).count();
}

static double cpu_seconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        throw std::runtime_error("GetProcessTimes failed");
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) / 10000000.0;
}

static double percentile(std::vector<double> numbers, double fraction) {
    if (numbers.empty()) return 0;
    std::sort(numbers.begin(), numbers.end());
    return numbers[static_cast<std::size_t>(std::ceil((numbers.size() - 1) * fraction))];
}

struct PresentLog {
    Clock::time_point origin{};
    std::mutex mutex;
    std::vector<double> times;
    std::vector<GstClockTime> pts;
    struct WindowState {
        HWND hwnd = nullptr;
        int found = -1;
        int visible = -1;
        int minimized = -1;
        int foreground = -1;
        int width = -1;
        int height = -1;
    };
    bool trace_window_state = false;
    HWND window_handle = nullptr;
    std::vector<WindowState> windows;
};

static thread_local GstClockTime active_sink_pts = GST_CLOCK_TIME_NONE;

struct StageEvent {
    const char* stage;
    double wall_ms;
    GstClockTime pts;
};

struct StageLog {
    Clock::time_point origin{};
    std::mutex mutex;
    std::vector<StageEvent> events;
};

struct StageTap {
    StageLog* log;
    const char* name;
};

// 診断時だけ挿入する素通し要素。下流gst_pad_pushの入口と戻りを同一PTSで測る。
struct GstTimedPush {
    GstElement parent;
    GstPad* sink_pad;
    GstPad* src_pad;
    StageLog* log;
};

struct GstTimedPushClass { GstElementClass parent_class; };

G_DEFINE_TYPE(GstTimedPush, gst_timed_push, GST_TYPE_ELEMENT)

static GstStaticPadTemplate timed_sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate timed_src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

static GstFlowReturn timed_push_chain(GstPad*, GstObject* parent, GstBuffer* buffer) {
    auto* self = reinterpret_cast<GstTimedPush*>(parent);
    const auto pts = GST_BUFFER_PTS(buffer);
    if (self->log) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> guard(self->log->mutex);
        self->log->events.push_back({"sink_push", ms(self->log->origin, now), pts});
    }
    const auto previous_pts = active_sink_pts;
    active_sink_pts = pts;
    const auto result = gst_pad_push(self->src_pad, buffer);
    active_sink_pts = previous_pts;
    if (self->log) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> guard(self->log->mutex);
        self->log->events.push_back({"sink_return", ms(self->log->origin, now), pts});
    }
    return result;
}

static gboolean timed_sink_event(GstPad*, GstObject* parent, GstEvent* event) {
    return gst_pad_push_event(reinterpret_cast<GstTimedPush*>(parent)->src_pad, event);
}

static gboolean timed_src_event(GstPad*, GstObject* parent, GstEvent* event) {
    return gst_pad_push_event(reinterpret_cast<GstTimedPush*>(parent)->sink_pad, event);
}

static gboolean timed_sink_query(GstPad*, GstObject* parent, GstQuery* query) {
    return gst_pad_peer_query(reinterpret_cast<GstTimedPush*>(parent)->src_pad, query);
}

static gboolean timed_src_query(GstPad*, GstObject* parent, GstQuery* query) {
    return gst_pad_peer_query(reinterpret_cast<GstTimedPush*>(parent)->sink_pad, query);
}

static void gst_timed_push_class_init(GstTimedPushClass* klass) {
    auto* element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "D3D11 display push meter", "Filter/Analyzer",
                                          "Measures downstream push return without changing buffers", "Tests");
    gst_element_class_add_static_pad_template(element, &timed_sink_template);
    gst_element_class_add_static_pad_template(element, &timed_src_template);
}

static void gst_timed_push_init(GstTimedPush* self) {
    self->sink_pad = gst_pad_new_from_static_template(&timed_sink_template, "sink");
    self->src_pad = gst_pad_new_from_static_template(&timed_src_template, "src");
    self->log = nullptr;
    gst_pad_set_chain_function(self->sink_pad, timed_push_chain);
    gst_pad_set_event_function(self->sink_pad, timed_sink_event);
    gst_pad_set_event_function(self->src_pad, timed_src_event);
    gst_pad_set_query_function(self->sink_pad, timed_sink_query);
    gst_pad_set_query_function(self->src_pad, timed_src_query);
    gst_element_add_pad(GST_ELEMENT(self), self->sink_pad);
    gst_element_add_pad(GST_ELEMENT(self), self->src_pad);
}

static GstPadProbeReturn on_stage(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto* tap = static_cast<StageTap*>(data);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> guard(tap->log->mutex);
        tap->log->events.push_back({tap->name, ms(tap->log->origin, now), GST_BUFFER_PTS(buffer)});
    }
    return GST_PAD_PROBE_OK;
}

static void add_stage_probe(GstElement* element, const char* pad_name, StageTap* tap) {
    auto* pad = gst_element_get_static_pad(element, pad_name);
    if (!pad) throw std::runtime_error("stage pad missing");
    if (!gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_stage, tap, nullptr)) {
        gst_object_unref(pad);
        throw std::runtime_error("cannot add stage probe");
    }
    gst_object_unref(pad);
}

static void sample_d3d11_window(HWND hwnd, PresentLog::WindowState* state) {
    state->hwnd = hwnd;
    state->found = 1;
    state->visible = IsWindowVisible(hwnd) ? 1 : 0;
    state->minimized = IsIconic(hwnd) ? 1 : 0;
    state->foreground = GetForegroundWindow() == hwnd ? 1 : 0;
    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        state->width = rect.right - rect.left;
        state->height = rect.bottom - rect.top;
    }
}

static BOOL CALLBACK find_d3d11_window(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    char class_name[32]{};
    if (!GetClassNameA(hwnd, class_name, sizeof(class_name)) ||
        std::string(class_name) != "GSTD3D11") return TRUE;
    sample_d3d11_window(hwnd, reinterpret_cast<PresentLog::WindowState*>(param));
    return FALSE;
}

static void on_present(GstElement*, GstObject*, gpointer, gpointer data) {
    auto* log = static_cast<PresentLog*>(data);
    PresentLog::WindowState window;
    if (log->trace_window_state) {
        window.found = 0;
        if (log->window_handle && IsWindow(log->window_handle))
            sample_d3d11_window(log->window_handle, &window);
        else {
            EnumWindows(find_d3d11_window, reinterpret_cast<LPARAM>(&window));
            log->window_handle = window.hwnd;
        }
    }
    const auto now = Clock::now();
    std::lock_guard<std::mutex> guard(log->mutex);
    log->times.push_back(ms(log->origin, now));
    log->pts.push_back(active_sink_pts);
    log->windows.push_back(window);
}

static std::uint64_t sink_stat(GstElement* sink, const char* name) {
    GstStructure* stats = nullptr;
    g_object_get(sink, "stats", &stats, nullptr);
    std::uint64_t value = 0;
    if (stats) {
        gst_structure_get_uint64(stats, name, &value);
        gst_structure_free(stats);
    }
    return value;
}

struct InjectedStall {
    Clock::time_point origin{};
    guint delay_ms = 0;
    bool fired = false;
    double start_ms = 0;
    double end_ms = 0;
};

static GstPadProbeReturn on_sink_stall(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto* stall = static_cast<InjectedStall*>(data);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer && !stall->fired && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) &&
        GST_BUFFER_PTS(buffer) >= GST_SECOND) {
        stall->fired = true;
        stall->start_ms = ms(stall->origin, Clock::now());
        Sleep(stall->delay_ms);
        stall->end_ms = ms(stall->origin, Clock::now());
    }
    return GST_PAD_PROBE_OK;
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    if (!gst_element_register(nullptr, "d3d11pushmeter", GST_RANK_NONE, gst_timed_push_get_type()))
        throw std::runtime_error("cannot register display push meter");
    if (argc < 4 || argc > 13)
        throw std::runtime_error("usage: d3d11_display_bench input.mov loops present.csv [stages.csv [preroll] [lossless] [native-rgb] [queue-before-decoder] [decoder-no-qos] [sink-stall-ms=N] [trace-sink-return] [trace-window-state]]");
    bool preroll = false;
    bool lossless = false;
    bool native_rgb = false;
    bool predecode_queue = false;
    bool decoder_no_qos = false;
    bool trace_sink_return = false;
    bool trace_window_state = false;
    guint sink_stall_ms = 0;
    for (int i = 5; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "preroll") preroll = true;
        else if (option == "lossless") lossless = true;
        else if (option == "native-rgb") native_rgb = true;
        else if (option == "queue-before-decoder") predecode_queue = true;
        else if (option == "decoder-no-qos") decoder_no_qos = true;
        else if (option == "trace-sink-return") trace_sink_return = true;
        else if (option == "trace-window-state") trace_window_state = true;
        else if (option.rfind("sink-stall-ms=", 0) == 0)
            sink_stall_ms = static_cast<guint>(std::stoi(option.substr(14)));
        else throw std::runtime_error("unknown display option: " + option);
    }
    if (sink_stall_ms > 1000) throw std::runtime_error("sink stall must be at most 1000ms");
    const int loops = std::stoi(argv[2]);
    if (loops < 1) throw std::runtime_error("loops must be positive");
    GError* error = nullptr;
    const std::string description = std::string("filesrc name=source ! qtdemux ! ") +
        (predecode_queue ? "queue name=predecode max-size-buffers=32 max-size-bytes=0 max-size-time=0 ! " : "") +
        "proresd3d11dec name=decoder ! " +
        (native_rgb ? "proresd3d11rgb" : "d3d11convert") +
        " name=converter ! video/x-raw(memory:D3D11Memory),format=RGB10A2_LE ! " +
        (trace_sink_return ? "d3d11pushmeter name=pushmeter ! " : "") +
        "d3d11videosink name=sink sync=true emit-present=true qos=true";
    auto* pipeline = gst_parse_launch(description.c_str(), &error);
    if (error || !pipeline) {
        const std::string text = error ? error->message : "cannot construct pipeline";
        if (error) g_error_free(error);
        throw std::runtime_error(text);
    }
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    auto* predecode = predecode_queue ? gst_bin_get_by_name(GST_BIN(pipeline), "predecode") : nullptr;
    auto* decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
    auto* converter = gst_bin_get_by_name(GST_BIN(pipeline), "converter");
    auto* pushmeter = trace_sink_return ? gst_bin_get_by_name(GST_BIN(pipeline), "pushmeter") : nullptr;
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    auto* bus = gst_element_get_bus(pipeline);
    if (!source || (predecode_queue && !predecode) || !decoder || !converter ||
        (trace_sink_return && !pushmeter) || !sink || !bus)
        throw std::runtime_error("pipeline endpoint missing");
    g_object_set(source, "location", argv[1], nullptr);
    if (decoder_no_qos) g_object_set(decoder, "qos", FALSE, nullptr);
    if (lossless) g_object_set(sink, "qos", FALSE, "max-lateness", gint64(-1), nullptr);
    gst_object_unref(source);
    PresentLog presents;
    presents.origin = Clock::now();
    presents.trace_window_state = trace_window_state;
    LARGE_INTEGER qpc_origin{}, qpc_frequency{};
    if (!QueryPerformanceFrequency(&qpc_frequency) || !QueryPerformanceCounter(&qpc_origin))
        throw std::runtime_error("QueryPerformanceCounter failed");
    InjectedStall stall;
    stall.origin = presents.origin;
    stall.delay_ms = sink_stall_ms;
    if (sink_stall_ms) {
        auto* pad = gst_element_get_static_pad(sink, "sink");
        if (!pad) throw std::runtime_error("sink pad missing for stall probe");
        const auto probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                                            on_sink_stall, &stall, nullptr);
        gst_object_unref(pad);
        if (!probe) throw std::runtime_error("cannot add sink stall probe");
    }
    StageLog stages;
    stages.origin = presents.origin;
    if (pushmeter) reinterpret_cast<GstTimedPush*>(pushmeter)->log = &stages;
    StageTap demuxed{&stages, "demuxed"};
    StageTap compressed{&stages, "compressed"};
    StageTap decoded{&stages, "decoded"};
    StageTap rgb{&stages, "rgb"};
    if (argc >= 5) {
        if (predecode) add_stage_probe(predecode, "sink", &demuxed);
        add_stage_probe(decoder, "sink", &compressed);
        add_stage_probe(decoder, "src", &decoded);
        add_stage_probe(converter, "src", &rgb);
    }
    g_signal_connect(sink, "present", G_CALLBACK(on_present), &presents);
    double preroll_ms = 0;
    if (preroll) {
        const auto preroll_start = Clock::now();
        if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE ||
            gst_element_get_state(pipeline, nullptr, nullptr, 10 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS)
            throw std::runtime_error("PAUSED preroll failed");
        preroll_ms = ms(preroll_start, Clock::now());
    }
    const auto start = Clock::now();
    const auto cpu_start = cpu_seconds();
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("PLAYING failed");
    std::vector<std::size_t> endpoints{0};
    std::vector<double> seek_first_ms;
    std::vector<std::uint64_t> rendered, dropped;
    std::uint64_t qos = 0;
    double seek_start = 0;
    for (int loop = 0; loop < loops;) {
        GstMessage* message = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_QOS));
        if (!message) throw std::runtime_error("display pipeline timeout");
        const auto type = GST_MESSAGE_TYPE(message);
        if (type == GST_MESSAGE_ERROR) {
            GError* gst_error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &gst_error, &debug);
            const std::string text = gst_error ? gst_error->message : "GStreamer error";
            g_clear_error(&gst_error);
            g_free(debug);
            gst_message_unref(message);
            throw std::runtime_error(text);
        }
        if (type == GST_MESSAGE_QOS) ++qos;
        if (type == GST_MESSAGE_EOS) {
            {
                std::lock_guard<std::mutex> guard(presents.mutex);
                endpoints.push_back(presents.times.size());
                if (loop > 0 && endpoints.back() > endpoints[endpoints.size() - 2])
                    seek_first_ms.push_back(presents.times[endpoints[endpoints.size() - 2]] - seek_start);
            }
            rendered.push_back(sink_stat(sink, "rendered"));
            dropped.push_back(sink_stat(sink, "dropped"));
            ++loop;
            if (loop < loops) {
                seek_start = ms(presents.origin, Clock::now());
                if (!gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), 0))
                    throw std::runtime_error("EOS seek failed");
            }
        }
        gst_message_unref(message);
    }
    const auto finish = Clock::now();
    const auto cpu_used = cpu_seconds() - cpu_start;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                              sizeof(memory))) throw std::runtime_error("GetProcessMemoryInfo failed");
    std::ofstream csv(argv[3]);
    if (!csv) throw std::runtime_error("cannot open present CSV");
    csv << "loop,present_index,wall_ms,interval_ms,pts_ns,window_found,window_visible,window_minimized,window_foreground,window_width,window_height\n";
    std::vector<double> intervals;
    for (int loop = 0; loop < loops; ++loop) {
        for (std::size_t i = endpoints[loop]; i < endpoints[loop + 1]; ++i) {
            const double interval = i > endpoints[loop] ? presents.times[i] - presents.times[i - 1] : 0;
            csv << loop << ',' << i - endpoints[loop] << ',' << presents.times[i] << ',' << interval << ',';
            if (GST_CLOCK_TIME_IS_VALID(presents.pts[i])) csv << presents.pts[i];
            const auto& window = presents.windows[i];
            csv << ',' << window.found << ',' << window.visible << ',' << window.minimized
                << ',' << window.foreground << ',' << window.width << ',' << window.height;
            csv << '\n';
            if (i > endpoints[loop] + 30) intervals.push_back(interval);
        }
    }
    if (argc >= 5) {
        std::ofstream stage_csv(argv[4]);
        if (!stage_csv) throw std::runtime_error("cannot open stages CSV");
        stage_csv << "stage,wall_ms,pts_ns\n";
        for (const auto& event : stages.events) {
            stage_csv << event.stage << ',' << event.wall_ms << ',';
            if (GST_CLOCK_TIME_IS_VALID(event.pts)) stage_csv << event.pts;
            stage_csv << '\n';
        }
    }
    const double wall = ms(start, finish);
    std::uint64_t total_rendered = 0, total_dropped = 0;
    for (auto value : rendered) total_rendered += value;
    for (auto value : dropped) total_dropped += value;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    std::cout << std::fixed << std::setprecision(3)
              << "{\"process_id\":" << GetCurrentProcessId()
              << ",\"qpc_origin_ms\":" << std::setprecision(6)
              << static_cast<double>(qpc_origin.QuadPart) * 1000.0 / qpc_frequency.QuadPart
              << std::setprecision(3)
              << ",\"loops\":" << loops << ",\"preroll_ms\":" << preroll_ms
              << ",\"rgb_converter\":\"" << (native_rgb ? "proresd3d11rgb" : "d3d11convert") << "\""
              << ",\"predecode_queue\":" << (predecode_queue ? "true" : "false")
              << ",\"decoder_no_qos\":" << (decoder_no_qos ? "true" : "false")
              << ",\"trace_sink_return\":" << (trace_sink_return ? "true" : "false")
              << ",\"trace_window_state\":" << (trace_window_state ? "true" : "false")
              << ",\"injected_sink_stall_ms\":" << sink_stall_ms
              << ",\"injected_stall_start_ms\":" << stall.start_ms
              << ",\"injected_stall_end_ms\":" << stall.end_ms
              << ",\"lossless_sink_policy\":" << (lossless ? "true" : "false")
              << ",\"present_count\":" << presents.times.size()
              << ",\"rendered\":" << total_rendered << ",\"dropped\":" << total_dropped
              << ",\"qos_messages\":" << qos
              << ",\"wall_ms\":" << wall
              << ",\"interval_p50_ms\":" << percentile(intervals, .5)
              << ",\"interval_p95_ms\":" << percentile(intervals, .95)
              << ",\"interval_p99_ms\":" << percentile(intervals, .99)
              << ",\"seek_first_p95_ms\":" << percentile(seek_first_ms, .95)
              << ",\"cpu_core_equivalent_percent\":" << cpu_used / (wall / 1000) * 100
              << ",\"cpu_machine_percent\":" << cpu_used / (wall / 1000) * 100 / system.dwNumberOfProcessors
              << ",\"peak_working_set_mib\":" << memory.PeakWorkingSetSize / 1048576.0
              << ",\"private_mib_end\":" << memory.PrivateUsage / 1048576.0
              << ",\"per_loop\":[";
    for (int loop = 0; loop < loops; ++loop) {
        if (loop) std::cout << ',';
        std::cout << "{\"present\":" << endpoints[loop + 1] - endpoints[loop]
                  << ",\"rendered\":" << rendered[loop]
                  << ",\"dropped\":" << dropped[loop] << '}';
    }
    std::cout << "]}\n";
    gst_object_unref(bus);
    if (predecode) gst_object_unref(predecode);
    if (pushmeter) gst_object_unref(pushmeter);
    gst_object_unref(decoder);
    gst_object_unref(converter);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "d3d11_display_bench: " << exception.what() << '\n';
    return 1;
}
