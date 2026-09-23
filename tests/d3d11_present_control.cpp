// 診断専用: 同じGStreamer D3D11MemoryフレームをPresent(0/1)で表示する。
// 標準d3d11videosinkの代替実装ではなく、DXGI同期値の影響を分離する対照。
#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void check_hr(HRESULT hr, const char* action) {
    if (SUCCEEDED(hr)) return;
    char message[128]{};
    sprintf_s(message, "%s failed: HRESULT 0x%08x", action, static_cast<unsigned>(hr));
    throw std::runtime_error(message);
}

struct DeviceLock {
    explicit DeviceLock(GstD3D11Device* device) : device(device) {
        gst_d3d11_device_lock(device);
    }
    ~DeviceLock() { gst_d3d11_device_unlock(device); }
    GstD3D11Device* device;
};

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wp, lp);
}

static void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

struct WindowState {
    int visible = 0;
    int minimized = 0;
    int foreground = 0;
    int topmost = 0;
    int foreground_overlap_percent = -1;
};

static WindowState sample_window(HWND hwnd) {
    WindowState state;
    state.visible = IsWindowVisible(hwnd) ? 1 : 0;
    state.minimized = IsIconic(hwnd) ? 1 : 0;
    state.topmost = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
    const HWND foreground = GetForegroundWindow();
    state.foreground = foreground == hwnd ? 1 : 0;
    if (state.foreground) state.foreground_overlap_percent = 100;
    else {
        RECT self{}, front{}, overlap{};
        if (foreground && GetWindowRect(hwnd, &self) && GetWindowRect(foreground, &front)) {
            const auto area = static_cast<std::int64_t>(self.right - self.left) *
                              (self.bottom - self.top);
            const auto covered = IntersectRect(&overlap, &self, &front)
                ? static_cast<std::int64_t>(overlap.right - overlap.left) *
                  (overlap.bottom - overlap.top)
                : 0;
            if (area > 0) state.foreground_overlap_percent =
                static_cast<int>(100 * covered / area);
        }
    }
    return state;
}

struct Window {
    HWND hwnd = nullptr;
    Window(int width, int height, bool topmost) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = instance;
        wc.lpszClassName = L"ProResDx11PresentControl";
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("RegisterClassW failed");
        const int client_width = width > 1280 ? 1280 : width;
        const int client_height = height > 720 ? 720 : height;
        hwnd = CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, wc.lpszClassName,
            L"DX11 ProRes Present diagnostic",
            WS_OVERLAPPEDWINDOW, 120, 120, client_width, client_height,
            nullptr, nullptr, instance, nullptr);
        require(hwnd != nullptr, "CreateWindowExW failed");
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
        if (topmost && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
            throw std::runtime_error("diagnostic window did not become topmost");
    }
    ~Window() { if (hwnd) DestroyWindow(hwnd); }
};

struct SwapChain {
    ComPtr<IDXGISwapChain3> chain;
    GstD3D11Device* gst_device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    int width = 0;
    int height = 0;

    SwapChain(GstD3D11Device* input_device, HWND hwnd, int input_width, int input_height)
        : gst_device(input_device),
          context(gst_d3d11_device_get_device_context_handle(input_device)),
          width(input_width), height(input_height) {
        auto* device = gst_d3d11_device_get_device_handle(gst_device);
        require(device && context, "GStreamer D3D11 device handles missing");
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        check_hr(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)), "Query IDXGIDevice");
        check_hr(dxgi_device->GetAdapter(&adapter), "Get DXGI adapter");
        check_hr(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get DXGI factory");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;
        check_hr(factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr,
                                                 &created), "CreateSwapChainForHwnd");
        check_hr(created.As(&chain), "Query IDXGISwapChain3");
        check_hr(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER),
                 "MakeWindowAssociation");
    }

    HRESULT present(GstMemory* memory, UINT interval) {
        require(gst_is_d3d11_memory(memory), "RGB output is not D3D11Memory");
        auto* d3d_memory = GST_D3D11_MEMORY_CAST(memory);
        require(d3d_memory->device == gst_device, "RGB output changed D3D11 device");
        ComPtr<ID3D11Texture2D> source;
        check_hr(gst_d3d11_memory_get_resource_handle(d3d_memory)->QueryInterface(
                     IID_PPV_ARGS(&source)), "Query RGB texture");
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        require(source_desc.Width == static_cast<UINT>(width) &&
                source_desc.Height == static_cast<UINT>(height) &&
                source_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM,
                "RGB texture format or dimensions changed");
        const UINT index = chain->GetCurrentBackBufferIndex();
        ComPtr<ID3D11Texture2D> back_buffer;
        check_hr(chain->GetBuffer(index, IID_PPV_ARGS(&back_buffer)), "Get swapchain buffer");
        DeviceLock lock(gst_device);
        context->CopyResource(back_buffer.Get(), source.Get());
        return chain->Present(interval, 0);
    }
};

static std::string bus_error(GstBus* bus) {
    GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (!message) return {};
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    std::string result = error ? error->message : "unknown GStreamer error";
    if (debug) result += std::string(" : ") + debug;
    g_clear_error(&error);
    g_free(debug);
    gst_message_unref(message);
    return result;
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    require(argc >= 4 && argc <= 6,
        "usage: d3d11_present_control input.mov|testsrc 0|1 output.csv [max_frames] [topmost]");
    const std::string input(argv[1]);
    const UINT interval = static_cast<UINT>(std::stoi(argv[2]));
    require(interval <= 1, "Present interval must be 0 or 1");
    int max_frames = 0;
    bool topmost = false;
    for (int arg = 4; arg < argc; ++arg) {
        if (std::string(argv[arg]) == "topmost") topmost = true;
        else max_frames = std::stoi(argv[arg]);
    }
    require(max_frames >= 0, "max_frames must not be negative");
    const bool testsrc = input == "testsrc";
    const std::string description = testsrc
        ? "d3d11testsrc num-buffers=480 ! "
          "video/x-raw(memory:D3D11Memory),format=RGB10A2_LE,"
          "width=3840,height=2160,framerate=60/1,colorimetry=bt709 ! "
          "appsink name=sink sync=false max-buffers=4"
        : "filesrc name=source ! qtdemux ! proresd3d11dec ! proresd3d11rgb ! "
          "video/x-raw(memory:D3D11Memory),format=RGB10A2_LE ! "
          "appsink name=sink sync=false max-buffers=4";
    GError* parse_error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &parse_error);
    if (parse_error || !pipeline) {
        const std::string message = parse_error ? parse_error->message : "pipeline missing";
        if (parse_error) g_error_free(parse_error);
        throw std::runtime_error(message);
    }
    auto* source = testsrc ? nullptr : gst_bin_get_by_name(GST_BIN(pipeline), "source");
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    auto* bus = gst_element_get_bus(pipeline);
    require(sink && bus && (testsrc || source), "pipeline endpoint missing");
    if (source) { g_object_set(source, "location", argv[1], nullptr); gst_object_unref(source); }
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "pipeline PLAYING failed");

    std::ofstream csv(argv[3]);
    require(csv.good(), "cannot open present CSV");
    csv << "index,pts_ns,before_copy_qpc,after_present_qpc,copy_present_ms,hresult,"
           "window_visible,window_minimized,window_foreground,window_topmost,"
           "foreground_overlap_percent\n";
    LARGE_INTEGER qpc_frequency{};
    require(QueryPerformanceFrequency(&qpc_frequency) != 0, "QPC frequency unavailable");
    std::unique_ptr<Window> window;
    std::unique_ptr<SwapChain> swapchain;
    GstClockTime first_pts = GST_CLOCK_TIME_NONE;
    Clock::time_point clock_origin{};
    int frames = 0;
    for (;;) {
        pump_messages();
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        if (!sample) {
            const auto error = bus_error(bus);
            if (!error.empty()) throw std::runtime_error(error);
            require(gst_app_sink_is_eos(GST_APP_SINK(sink)), "RGB sample timeout");
            break;
        }
        auto* buffer = gst_sample_get_buffer(sample);
        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        require(GST_CLOCK_TIME_IS_VALID(pts), "RGB PTS missing");
        require(gst_buffer_n_memory(buffer) == 1, "RGB output must have one memory");
        if (!swapchain) {
            GstVideoInfo info{};
            require(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)),
                    "RGB caps invalid");
            require(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_RGB10A2_LE,
                    "RGB format mismatch");
            window = std::make_unique<Window>(GST_VIDEO_INFO_WIDTH(&info),
                                               GST_VIDEO_INFO_HEIGHT(&info), topmost);
            auto* raw_memory = gst_buffer_peek_memory(buffer, 0);
            require(gst_is_d3d11_memory(raw_memory), "RGB output is not D3D11Memory");
            auto* memory = GST_D3D11_MEMORY_CAST(raw_memory);
            swapchain = std::make_unique<SwapChain>(memory->device, window->hwnd,
                GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info));
            first_pts = pts;
            clock_origin = Clock::now();
        }
        const auto target = clock_origin + std::chrono::nanoseconds(pts - first_pts);
        std::this_thread::sleep_until(target);
        pump_messages();
        const auto window_state = sample_window(window->hwnd);
        LARGE_INTEGER before{}, after{};
        QueryPerformanceCounter(&before);
        const HRESULT hr = swapchain->present(gst_buffer_peek_memory(buffer, 0), interval);
        QueryPerformanceCounter(&after);
        csv << frames << ',' << pts << ',' << before.QuadPart << ',' << after.QuadPart << ','
            << std::fixed << std::setprecision(4)
            << (after.QuadPart - before.QuadPart) * 1000.0 / qpc_frequency.QuadPart << ','
            << static_cast<unsigned>(hr) << ',' << window_state.visible << ','
            << window_state.minimized << ',' << window_state.foreground << ','
            << window_state.topmost << ',' << window_state.foreground_overlap_percent << '\n';
        gst_sample_unref(sample);
        check_hr(hr, "DXGI Present");
        ++frames;
        if (max_frames && frames >= max_frames) break;
    }
    csv.flush();
    require(csv.good(), "present CSV write failed");
    std::cout << "{\"passed\":true,\"source\":\"" << (testsrc ? "testsrc" : "prores")
              << "\",\"interval\":" << interval << ",\"frames\":" << frames
              << ",\"qpc_frequency\":" << qpc_frequency.QuadPart
              << ",\"width\":" << swapchain->width << ",\"height\":" << swapchain->height
              << ",\"swap_effect\":\"flip_discard\",\"buffer_count\":3,"
                 "\"topmost\":" << (topmost ? "true" : "false") << "}\n";
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    gst_deinit();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
