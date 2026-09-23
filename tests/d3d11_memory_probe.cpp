// Verify the actual GStreamer D3D11Memory topology selected for I422_10LE.
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideopool.h>
#include <gst/d3d11/gstd3d11bufferpool.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11format.h>
#include <gst/d3d11/gstd3d11memory.h>

#include <d3d11.h>

#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv);
    GstD3D11Device* device = gst_d3d11_device_new(0, 0);
    require(device != nullptr, "cannot create GstD3D11Device");
    GstD3D11Format format{};
    require(gst_d3d11_device_get_format(device, GST_VIDEO_FORMAT_I422_10LE, &format),
            "I422_10LE is unsupported by GstD3D11Device");

    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I422_10LE",
        "width", G_TYPE_INT, 1920,
        "height", G_TYPE_INT, 1080,
        "framerate", GST_TYPE_FRACTION, 60, 1,
        "interlace-mode", G_TYPE_STRING, "progressive", nullptr);
    gst_caps_set_features(caps, 0,
        gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, nullptr));
    GstVideoInfo info{};
    require(gst_video_info_from_caps(&info, caps), "invalid probe caps");
    GstBufferPool* pool = gst_d3d11_buffer_pool_new(device);
    require(pool != nullptr, "cannot create D3D11 buffer pool");
    GstStructure* config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, caps, info.size, 2, 4);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
    GstD3D11AllocationParams* params = gst_d3d11_allocation_params_new(
        device, &info, GST_D3D11_ALLOCATION_FLAG_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0);
    require(params != nullptr, "cannot create allocation parameters");
    gst_buffer_pool_config_set_d3d11_allocation_params(config, params);
    gst_d3d11_allocation_params_free(params);
    require(gst_buffer_pool_set_config(pool, config), "D3D11 pool rejected I422_10LE UAV config");
    require(gst_buffer_pool_set_active(pool, TRUE), "cannot activate D3D11 pool");
    GstBuffer* buffer = nullptr;
    require(gst_buffer_pool_acquire_buffer(pool, &buffer, nullptr) == GST_FLOW_OK,
            "cannot acquire D3D11 buffer");

    auto* handle = gst_d3d11_device_get_device_handle(device);
    UINT rgb_support = 0;
    require(SUCCEEDED(handle->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &rgb_support)),
            "RGB10A2 DXGI format query failed");
    const bool rgb_typed_uav = (rgb_support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    const auto memories = gst_buffer_n_memory(buffer);
    require(memories == 3, "I422_10LE must allocate exactly three D3D11 memories");
    bool all_uav = true;
    std::cout << "{\"passed\":true,\"gst_format\":\"I422_10LE\",\"memories\":"
              << memories << ",\"device_dxgi_format\":" << static_cast<unsigned>(format.dxgi_format)
              << ",\"planes\":[";
    for (guint i = 0; i < memories; ++i) {
        GstMemory* memory = gst_buffer_peek_memory(buffer, i);
        require(gst_is_d3d11_memory(memory), "pool returned non-D3D11 memory");
        auto* d3d_memory = GST_D3D11_MEMORY_CAST(memory);
        D3D11_TEXTURE2D_DESC desc{};
        require(gst_d3d11_memory_get_texture_desc(d3d_memory, &desc), "missing texture desc");
        ID3D11Resource* resource = gst_d3d11_memory_get_resource_handle(d3d_memory);
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = desc.Format;
        uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ID3D11UnorderedAccessView* uav = nullptr;
        const HRESULT result = handle->CreateUnorderedAccessView(resource, &uav_desc, &uav);
        if (uav) uav->Release();
        all_uav = all_uav && SUCCEEDED(result);
        if (i) std::cout << ',';
        std::cout << "{\"index\":" << i
                  << ",\"width\":" << desc.Width << ",\"height\":" << desc.Height
                  << ",\"dxgi_format\":" << static_cast<unsigned>(desc.Format)
                  << ",\"bind_flags\":" << desc.BindFlags
                  << ",\"uav_create\":" << (SUCCEEDED(result) ? "true" : "false") << '}';
    }
    std::cout << "],\"all_planes_uav\":" << (all_uav ? "true" : "false")
              << ",\"rgb10a2_typed_uav\":" << (rgb_typed_uav ? "true" : "false") << "}\n";
    require(all_uav, "one or more output planes cannot be bound as UAV");

    gst_buffer_unref(buffer);
    gst_buffer_pool_set_active(pool, FALSE);
    gst_object_unref(pool);
    gst_caps_unref(caps);
    gst_object_unref(device);
    gst_deinit();
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
