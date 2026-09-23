// 復号済みI422_10LE D3D11Memoryを独立BT.709 shaderでRGB10A2へ変換する単体検証器。
#include <gst/app/gstappsink.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

static void check_hr(HRESULT result, const char* reason) {
    if (FAILED(result)) throw std::runtime_error(std::string(reason) + " HRESULT=" +
                                                  std::to_string(static_cast<unsigned long>(result)));
}

struct DeviceLock {
    explicit DeviceLock(GstD3D11Device* value) : device(value) { gst_d3d11_device_lock(device); }
    ~DeviceLock() { gst_d3d11_device_unlock(device); }
    GstD3D11Device* device;
};

struct Parameters {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t reserved[2]{};
};

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    require(argc == 4 || argc == 5,
            "usage: d3d11_rgb_probe input.mov output.raw prores_rgb.cso [frames; 0=all]");
    const int frame_limit = argc == 5 ? std::stoi(argv[4]) : 1;
    require(frame_limit >= 0, "frame limit must be nonnegative");
    GError* error = nullptr;
    auto* pipeline = gst_parse_launch(
        "filesrc name=source ! qtdemux ! proresd3d11dec ! "
        "appsink name=sink sync=false max-buffers=1", &error);
    if (error || !pipeline) {
        const std::string message = error ? error->message : "pipeline construction failed";
        if (error) g_error_free(error);
        throw std::runtime_error(message);
    }
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    require(source && sink, "pipeline endpoint missing");
    g_object_set(source, "location", argv[1], nullptr);
    gst_object_unref(source);
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "pipeline PLAYING failed");
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
    require(sample != nullptr, "DX11 frame timeout");
    auto* caps = gst_sample_get_caps(sample);
    GstVideoInfo info{};
    require(gst_video_info_from_caps(&info, caps), "invalid decoded caps");
    require(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_I422_10LE &&
            info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235 &&
            info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709 &&
            info.colorimetry.transfer == GST_VIDEO_TRANSFER_BT709 &&
            info.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_BT709 &&
            gst_caps_features_contains(gst_caps_get_features(caps, 0),
                                       GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY),
            "expected limited BT.709 I422_10LE D3D11Memory");
    auto* buffer = gst_sample_get_buffer(sample);
    require(gst_buffer_n_memory(buffer) == 3, "expected three I422 textures");
    auto* first = GST_D3D11_MEMORY_CAST(gst_buffer_peek_memory(buffer, 0));
    require(gst_is_d3d11_memory(&first->mem), "first plane is not D3D11Memory");
    auto* gst_device = first->device;
    auto* device = gst_d3d11_device_get_device_handle(gst_device);
    auto* context = gst_d3d11_device_get_device_context_handle(gst_device);
    require(device && context, "missing native D3D11 handles");

    std::ifstream shader_file(argv[3], std::ios::binary | std::ios::ate);
    require(static_cast<bool>(shader_file), "cannot open RGB shader");
    const auto shader_size = shader_file.tellg();
    require(shader_size > 0 && shader_size < 1024 * 1024, "invalid RGB shader bytecode");
    std::vector<char> shader_bytes(static_cast<std::size_t>(shader_size));
    shader_file.seekg(0);
    shader_file.read(shader_bytes.data(), static_cast<std::streamsize>(shader_bytes.size()));
    require(static_cast<bool>(shader_file), "cannot read RGB shader");
    ComPtr<ID3D11ComputeShader> shader;
    check_hr(device->CreateComputeShader(shader_bytes.data(), shader_bytes.size(), nullptr, &shader),
             "CreateComputeShader");

    D3D11_TEXTURE2D_DESC rgb_desc{};
    rgb_desc.Width = static_cast<UINT>(info.width);
    rgb_desc.Height = static_cast<UINT>(info.height);
    rgb_desc.MipLevels = 1;
    rgb_desc.ArraySize = 1;
    rgb_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    rgb_desc.SampleDesc.Count = 1;
    rgb_desc.Usage = D3D11_USAGE_DEFAULT;
    rgb_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> rgb;
    check_hr(device->CreateTexture2D(&rgb_desc, nullptr, &rgb), "Create RGB texture");
    ComPtr<ID3D11UnorderedAccessView> output;
    check_hr(device->CreateUnorderedAccessView(rgb.Get(), nullptr, &output), "Create RGB UAV");
    Parameters params{rgb_desc.Width, rgb_desc.Height};
    D3D11_BUFFER_DESC constant_desc{};
    constant_desc.ByteWidth = sizeof(Parameters);
    constant_desc.Usage = D3D11_USAGE_DEFAULT;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constant_data{&params, 0, 0};
    ComPtr<ID3D11Buffer> constants;
    check_hr(device->CreateBuffer(&constant_desc, &constant_data, &constants), "Create RGB constants");
    D3D11_TEXTURE2D_DESC staging_desc = rgb_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check_hr(device->CreateTexture2D(&staging_desc, nullptr, &staging), "Create RGB staging");

    std::ofstream raw(argv[2], std::ios::binary);
    require(static_cast<bool>(raw), "cannot open RGB raw output");
    std::uint64_t frame_count = 0;
    while (sample) {
        GstVideoInfo current{};
        auto* current_caps = gst_sample_get_caps(sample);
        require(gst_video_info_from_caps(&current, current_caps) &&
                current.width == info.width && current.height == info.height &&
                GST_VIDEO_INFO_FORMAT(&current) == GST_VIDEO_FORMAT_I422_10LE &&
                current.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235 &&
                current.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709 &&
                current.colorimetry.transfer == GST_VIDEO_TRANSFER_BT709 &&
                current.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_BT709 &&
                gst_caps_features_contains(gst_caps_get_features(current_caps, 0),
                                           GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY),
                "input colorimetry or format changed during conversion");
        auto* current_buffer = gst_sample_get_buffer(sample);
        require(gst_buffer_n_memory(current_buffer) == 3, "expected three I422 textures");
        ComPtr<ID3D11ShaderResourceView> inputs[3];
        for (guint component = 0; component < 3; ++component) {
            auto* memory = gst_buffer_peek_memory(current_buffer, component);
            require(gst_is_d3d11_memory(memory), "non-D3D11 input plane");
            auto* d3d_memory = GST_D3D11_MEMORY_CAST(memory);
            require(d3d_memory->device == gst_device, "input planes belong to different devices");
            D3D11_TEXTURE2D_DESC texture{};
            require(gst_d3d11_memory_get_texture_desc(d3d_memory, &texture) &&
                    texture.Format == DXGI_FORMAT_R16_UNORM && texture.ArraySize == 1 &&
                    texture.Width == static_cast<UINT>(component ? info.width / 2 : info.width) &&
                    texture.Height == static_cast<UINT>(info.height),
                    "unexpected I422 plane topology");
            check_hr(device->CreateShaderResourceView(
                gst_d3d11_memory_get_resource_handle(d3d_memory), nullptr, &inputs[component]),
                "CreateShaderResourceView");
        }
        {
            DeviceLock lock(gst_device);
            ID3D11ShaderResourceView* views[] = {inputs[0].Get(), inputs[1].Get(), inputs[2].Get()};
            ID3D11UnorderedAccessView* uavs[] = {output.Get()};
            ID3D11Buffer* constant_buffers[] = {constants.Get()};
            context->CSSetShaderResources(0, 3, views);
            context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            context->CSSetConstantBuffers(0, 1, constant_buffers);
            context->CSSetShader(shader.Get(), nullptr, 0);
            context->Dispatch((rgb_desc.Width + 7) / 8, (rgb_desc.Height + 7) / 8, 1);
            ID3D11ShaderResourceView* null_views[] = {nullptr, nullptr, nullptr};
            ID3D11UnorderedAccessView* null_uavs[] = {nullptr};
            ID3D11Buffer* null_constants[] = {nullptr};
            context->CSSetShader(nullptr, nullptr, 0);
            context->CSSetShaderResources(0, 3, null_views);
            context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
            context->CSSetConstantBuffers(0, 1, null_constants);
            context->CopyResource(staging.Get(), rgb.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check_hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                     "Map RGB staging");
            for (UINT y = 0; y < rgb_desc.Height; ++y) {
                const auto* row = static_cast<const char*>(mapped.pData) + y * mapped.RowPitch;
                raw.write(row, static_cast<std::streamsize>(rgb_desc.Width * 4));
            }
            context->Unmap(staging.Get(), 0);
            require(static_cast<bool>(raw), "RGB raw output write failed");
        }
        gst_sample_unref(sample);
        ++frame_count;
        if (frame_limit && frame_count >= static_cast<std::uint64_t>(frame_limit)) break;
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND);
        if (!sample) require(gst_app_sink_is_eos(GST_APP_SINK(sink)), "DX11 frame timeout or error");
    }
    raw.flush();
    require(static_cast<bool>(raw), "RGB raw output flush failed");
    std::cout << "{\"width\":" << info.width << ",\"height\":" << info.height
              << ",\"frames\":" << frame_count
              << ",\"format\":\"RGB10A2_LE\",\"memory\":\"D3D11\"}\n";
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "d3d11_rgb_probe: " << exception.what() << '\n';
    return 1;
}
