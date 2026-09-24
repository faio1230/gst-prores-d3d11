/* Native D3D11 planar 422/444 10/12-bit -> RGB10A2_LE color converter.
 * The compute shader writes directly into a GstD3D11BufferPool texture.
 */
#include "d3d11_hardware_device.hpp"
#include <gst/base/gstbasetransform.h>
#include <gst/d3d11/gstd3d11bufferpool.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/video/video.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

void check_hr(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " HRESULT=" +
                                                  std::to_string(static_cast<unsigned long>(result)));
}

HRESULT device_removed_reason(GstD3D11Device* gst_device) {
    auto* device = gst_device ? gst_d3d11_device_get_device_handle(gst_device) : nullptr;
    return device ? device->GetDeviceRemovedReason() : S_OK;
}

std::filesystem::path shader_path(bool alpha) {
    static int anchor;
    HMODULE module = nullptr;
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&anchor), &module) != 0,
            "cannot locate RGB converter module");
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    require(length && length < path.size(), "cannot resolve RGB converter module path");
    return std::filesystem::path(path.data(), path.data() + length).parent_path() /
           (alpha ? L"prores_rgb_alpha.cso" : L"prores_rgb.cso");
}

class DeviceLock {
public:
    explicit DeviceLock(GstD3D11Device* device) : device_(device) { gst_d3d11_device_lock(device_); }
    ~DeviceLock() { gst_d3d11_device_unlock(device_); }
private:
    GstD3D11Device* device_;
};

struct Parameters {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mode;
    std::uint32_t reserved = 0;
};

class RgbBackend {
public:
    RgbBackend(GstD3D11Device* gst_device, unsigned width, unsigned height,
               unsigned chroma_shift, unsigned bit_depth, bool alpha)
        : gst_device_(gst_device), device_(gst_d3d11_device_get_device_handle(gst_device)),
          context_(gst_d3d11_device_get_device_context_handle(gst_device)),
          width_(width), height_(height), chroma_shift_(chroma_shift), alpha_(alpha) {
        require(device_ && context_, "missing native D3D11 handles");
        require_d3d11_non_software_adapter(device_);
        require(device_->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0,
                "RGB Compute Shader requires D3D feature level 11_0 or higher");
        UINT input_support = 0;
        check_hr(device_->CheckFormatSupport(alpha ? DXGI_FORMAT_R16G16B16A16_UNORM :
                                            DXGI_FORMAT_R16_UNORM, &input_support),
                 "CheckFormatSupport RGB input");
        constexpr UINT required_input = D3D11_FORMAT_SUPPORT_TEXTURE2D |
            D3D11_FORMAT_SUPPORT_SHADER_LOAD;
        require((input_support & required_input) == required_input,
                "R16_UNORM texture shader load unsupported by this adapter");
        UINT support = 0;
        check_hr(device_->CheckFormatSupport(alpha ? DXGI_FORMAT_R16G16B16A16_UNORM :
                                            DXGI_FORMAT_R10G10B10A2_UNORM, &support),
                 "CheckFormatSupport RGB output");
        constexpr UINT required_output = D3D11_FORMAT_SUPPORT_TEXTURE2D |
            D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_RENDER_TARGET |
            D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
        require((support & required_output) == required_output,
                "RGB10A2 texture/load/render target/typed UAV unsupported by this adapter");
        std::ifstream file(shader_path(alpha), std::ios::binary | std::ios::ate);
        require(static_cast<bool>(file), "cannot open RGB shader");
        const auto size = file.tellg();
        require(size > 0 && size < 1024 * 1024, "invalid RGB shader size");
        std::vector<char> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(file), "cannot read RGB shader");
        check_hr(device_->CreateComputeShader(bytes.data(), bytes.size(), nullptr, &shader_),
                 "CreateComputeShader RGB");
        Parameters parameters{width, height, (bit_depth << 8) | chroma_shift};
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(Parameters);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{&parameters, 0, 0};
        check_hr(device_->CreateBuffer(&desc, &data, &constants_), "CreateBuffer RGB constants");
    }

    void convert(GstBuffer* input, GstBuffer* output) {
        check_hr(device_->GetDeviceRemovedReason(), "D3D11 device removed before RGB conversion");
        require(gst_buffer_n_memory(input) == (alpha_ ? 1u : 3u) &&
                gst_buffer_n_memory(output) == 1,
                "RGB converter input/output memory count changed");
        ComPtr<ID3D11ShaderResourceView> inputs[3];
        for (guint index = 0; index < (alpha_ ? 1u : 3u); ++index) {
            auto* memory = gst_buffer_peek_memory(input, index);
            require(gst_is_d3d11_memory(memory), "RGB input is not D3D11Memory");
            auto* d3d_memory = GST_D3D11_MEMORY_CAST(memory);
            require(d3d_memory->device == gst_device_, "RGB input device changed");
            D3D11_TEXTURE2D_DESC desc{};
            const bool has_desc = gst_d3d11_memory_get_texture_desc(d3d_memory, &desc);
            if (!has_desc || desc.Format != (alpha_ ? DXGI_FORMAT_R16G16B16A16_UNORM :
                                             DXGI_FORMAT_R16_UNORM) || desc.ArraySize != 1 ||
                desc.Width != (alpha_ || !index ? width_ : width_ >> chroma_shift_) ||
                desc.Height != height_ ||
                !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
                throw std::runtime_error("RGB input plane topology is unexpected: plane=" +
                    std::to_string(index) + " format=" + std::to_string(desc.Format) +
                    " array=" + std::to_string(desc.ArraySize) +
                    " size=" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                    " bind=" + std::to_string(desc.BindFlags) +
                    " subresource=" +
                    std::to_string(gst_d3d11_memory_get_subresource_index(d3d_memory)));
            check_hr(device_->CreateShaderResourceView(
                gst_d3d11_memory_get_resource_handle(d3d_memory), nullptr, &inputs[index]),
                "CreateShaderResourceView RGB input");
        }
        auto* memory = gst_buffer_peek_memory(output, 0);
        require(gst_is_d3d11_memory(memory), "RGB output is not D3D11Memory");
        auto* d3d_memory = GST_D3D11_MEMORY_CAST(memory);
        require(d3d_memory->device == gst_device_, "RGB output device changed");
        D3D11_TEXTURE2D_DESC desc{};
        require(gst_d3d11_memory_get_texture_desc(d3d_memory, &desc) &&
                desc.Format == (alpha_ ? DXGI_FORMAT_R16G16B16A16_UNORM :
                                DXGI_FORMAT_R10G10B10A2_UNORM) && desc.ArraySize == 1 &&
                desc.Width == width_ && desc.Height == height_ &&
                (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS),
                "RGB output texture topology is unexpected");
        ComPtr<ID3D11UnorderedAccessView> output_view;
        check_hr(device_->CreateUnorderedAccessView(
            gst_d3d11_memory_get_resource_handle(d3d_memory), nullptr, &output_view),
            "CreateUnorderedAccessView RGB output");
        {
            DeviceLock lock(gst_device_);
            ID3D11ShaderResourceView* views[] = {inputs[0].Get(), inputs[1].Get(), inputs[2].Get()};
            ID3D11UnorderedAccessView* uavs[] = {output_view.Get()};
            ID3D11Buffer* constants[] = {constants_.Get()};
            context_->CSSetShaderResources(0, alpha_ ? 1u : 3u, views);
            context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
            context_->CSSetConstantBuffers(0, 1, constants);
            context_->CSSetShader(shader_.Get(), nullptr, 0);
            context_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
            ID3D11ShaderResourceView* null_views[] = {nullptr, nullptr, nullptr};
            ID3D11UnorderedAccessView* null_uavs[] = {nullptr};
            ID3D11Buffer* null_constants[] = {nullptr};
            context_->CSSetShader(nullptr, nullptr, 0);
            context_->CSSetShaderResources(0, alpha_ ? 1u : 3u, null_views);
            context_->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
            context_->CSSetConstantBuffers(0, 1, null_constants);
            GST_MEMORY_FLAG_UNSET(memory, GST_D3D11_MEMORY_TRANSFER_NEED_UPLOAD);
            GST_MINI_OBJECT_FLAG_SET(memory, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD);
        }
    }

private:
    GstD3D11Device* gst_device_;
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    unsigned width_;
    unsigned height_;
    unsigned chroma_shift_;
    bool alpha_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constants_;
};

}  // namespace

typedef struct _GstProresD3D11Rgb {
    GstBaseTransform parent;
    GstVideoInfo input_info;
    GstVideoInfo output_info;
    GstCaps* output_caps;
    GstD3D11Device* device;
    GstBufferPool* pool;
    RgbBackend* backend;
    guint input_bit_depth;
    guint input_chroma_shift;
    gboolean input_has_alpha;
} GstProresD3D11Rgb;

typedef struct _GstProresD3D11RgbClass { GstBaseTransformClass parent_class; } GstProresD3D11RgbClass;
G_DEFINE_TYPE(GstProresD3D11Rgb, gst_prores_d3d11_rgb, GST_TYPE_BASE_TRANSFORM)
#define SELF(obj) (reinterpret_cast<GstProresD3D11Rgb*>(obj))

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:D3D11Memory), "
                    "format=(string){ I422_10LE, Y444_10LE, I422_12LE, Y444_12LE, AYUV64 }, "
                    "width=(int)[16,8192], height=(int)[16,8192]"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:D3D11Memory), "
                    "format=(string){ RGB10A2_LE, RGBA64_LE }, "
                    "width=(int)[16,8192], height=(int)[16,8192]"));

static void reset_gpu(GstProresD3D11Rgb* self) {
    delete self->backend;
    self->backend = nullptr;
    if (self->pool) {
        gst_buffer_pool_set_active(self->pool, FALSE);
        gst_clear_object(&self->pool);
    }
    if (self->device) gst_clear_object(&self->device);
}

static GstCaps* transform_caps(GstBaseTransform*, GstPadDirection direction,
                               GstCaps* caps, GstCaps* filter) {
    auto* result = gst_caps_copy(caps);
    for (guint index = 0; index < gst_caps_get_size(result); ++index) {
        auto* structure = gst_caps_get_structure(result, index);
        gst_structure_set(structure, "colorimetry", G_TYPE_STRING,
                          direction == GST_PAD_SINK ? "1:1:5:1" : "bt709", nullptr);
        if (direction == GST_PAD_SINK) {
            const char* source_format = gst_structure_get_string(structure, "format");
            if (source_format) {
                gst_structure_set(structure, "format", G_TYPE_STRING,
                                  g_strcmp0(source_format, "AYUV64") == 0 ?
                                      "RGBA64_LE" : "RGB10A2_LE", nullptr);
            } else {
                GValue formats = G_VALUE_INIT;
                g_value_init(&formats, GST_TYPE_LIST);
                for (const char* name : {"RGB10A2_LE", "RGBA64_LE"}) {
                    GValue value = G_VALUE_INIT;
                    g_value_init(&value, G_TYPE_STRING);
                    g_value_set_string(&value, name);
                    gst_value_list_append_value(&formats, &value);
                    g_value_unset(&value);
                }
                gst_structure_set_value(structure, "format", &formats);
                g_value_unset(&formats);
            }
            gst_structure_remove_field(structure, "prores-depth");
            gst_structure_remove_field(structure, "prores-chroma-shift");
        } else {
            GValue formats = G_VALUE_INIT;
            g_value_init(&formats, GST_TYPE_LIST);
            const char* target_format = gst_structure_get_string(structure, "format");
            const bool alpha = target_format && g_strcmp0(target_format, "RGBA64_LE") == 0;
            const bool all = target_format == nullptr;
            for (const char* name : {"I422_10LE", "Y444_10LE", "I422_12LE", "Y444_12LE", "AYUV64"}) {
                if (alpha && g_strcmp0(name, "AYUV64") != 0) continue;
                if (!alpha && !all && g_strcmp0(name, "AYUV64") == 0) continue;
                GValue value = G_VALUE_INIT;
                g_value_init(&value, G_TYPE_STRING);
                g_value_set_string(&value, name);
                gst_value_list_append_value(&formats, &value);
                g_value_unset(&value);
            }
            gst_structure_set_value(structure, "format", &formats);
            g_value_unset(&formats);
        }
        gst_structure_remove_field(structure, "chroma-site");
    }
    if (filter) {
        auto* intersection = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(result);
        result = intersection;
    }
    return result;
}

static gboolean set_caps(GstBaseTransform* transform, GstCaps* input, GstCaps* output) {
    auto* self = SELF(transform);
    GstVideoInfo in{};
    GstVideoInfo out{};
    const char* chroma_site = gst_structure_get_string(gst_caps_get_structure(input, 0),
                                                        "chroma-site");
    if (!gst_video_info_from_caps(&in, input) || !gst_video_info_from_caps(&out, output))
        return FALSE;
    const auto format = GST_VIDEO_INFO_FORMAT(&in);
    const bool alpha = format == GST_VIDEO_FORMAT_AYUV64;
    int alpha_depth = 0;
    int alpha_chroma_shift = -1;
    if (alpha) gst_structure_get_int(gst_caps_get_structure(input, 0),
                                     "prores-depth", &alpha_depth);
    if (alpha) gst_structure_get_int(gst_caps_get_structure(input, 0),
                                     "prores-chroma-shift", &alpha_chroma_shift);
    if ((format != GST_VIDEO_FORMAT_I422_10LE &&
         format != GST_VIDEO_FORMAT_Y444_10LE &&
         format != GST_VIDEO_FORMAT_I422_12LE &&
         format != GST_VIDEO_FORMAT_Y444_12LE && !alpha) ||
        GST_VIDEO_INFO_FORMAT(&out) != (alpha ? GST_VIDEO_FORMAT_RGBA64_LE :
                                        GST_VIDEO_FORMAT_RGB10A2_LE) ||
        (alpha && alpha_depth != 10 && alpha_depth != 12) ||
        (alpha && alpha_chroma_shift != 0 && alpha_chroma_shift != 1) ||
        in.width != out.width || in.height != out.height || (in.width & 1) ||
        in.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
        out.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
        (chroma_site && std::strcmp(chroma_site, "jpeg") != 0) ||
        in.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235 ||
        in.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709 ||
        in.colorimetry.transfer != GST_VIDEO_TRANSFER_BT709 ||
        in.colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_BT709 ||
        out.colorimetry.range != GST_VIDEO_COLOR_RANGE_0_255 ||
        out.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_RGB ||
        out.colorimetry.transfer != GST_VIDEO_TRANSFER_BT709 ||
        out.colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_BT709 ||
        !gst_caps_features_contains(gst_caps_get_features(input, 0),
                                    GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY) ||
        !gst_caps_features_contains(gst_caps_get_features(output, 0),
                                    GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY)) {
        GST_ERROR_OBJECT(self, "expected progressive limited BT.709 planar or AYUV64 ProRes -> matching RGB10A2/RGBA64 D3D11Memory");
        return FALSE;
    }
    reset_gpu(self);
    if (self->output_caps) gst_caps_unref(self->output_caps);
    self->output_caps = gst_caps_ref(output);
    self->input_info = in;
    self->output_info = out;
    self->input_has_alpha = alpha;
    self->input_bit_depth = alpha ? static_cast<guint>(alpha_depth) :
        (format == GST_VIDEO_FORMAT_I422_12LE || format == GST_VIDEO_FORMAT_Y444_12LE ? 12u : 10u);
    self->input_chroma_shift = alpha ? static_cast<guint>(alpha_chroma_shift) :
        (format == GST_VIDEO_FORMAT_I422_10LE || format == GST_VIDEO_FORMAT_I422_12LE ? 1u : 0u);
    return TRUE;
}

static void ensure_output_pool(GstProresD3D11Rgb* self, GstBuffer* input) {
    require(self->output_caps != nullptr &&
            gst_buffer_n_memory(input) == (self->input_has_alpha ? 1u : 3u),
            "RGB converter is not negotiated or input topology changed");
    auto* memory = gst_buffer_peek_memory(input, 0);
    require(gst_is_d3d11_memory(memory), "RGB input is not D3D11Memory");
    auto* device = GST_D3D11_MEMORY_CAST(memory)->device;
    if (self->pool && self->device == device) return;
    reset_gpu(self);
    self->device = GST_D3D11_DEVICE(gst_object_ref(device));
    self->backend = new RgbBackend(device, self->input_info.width, self->input_info.height,
                                   self->input_chroma_shift, self->input_bit_depth,
                                   self->input_has_alpha);
    self->pool = gst_d3d11_buffer_pool_new(device);
    require(self->pool != nullptr, "cannot create RGB D3D11 buffer pool");
    auto* config = gst_buffer_pool_get_config(self->pool);
    gst_buffer_pool_config_set_params(config, self->output_caps,
                                      static_cast<guint>(self->output_info.size), 3, 0);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
    auto* params = gst_d3d11_allocation_params_new(device, &self->output_info,
        GST_D3D11_ALLOCATION_FLAG_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, 0);
    require(params != nullptr, "cannot create RGB D3D11 allocation parameters");
    gst_buffer_pool_config_set_d3d11_allocation_params(config, params);
    gst_d3d11_allocation_params_free(params);
    require(gst_buffer_pool_set_config(self->pool, config), "cannot configure RGB D3D11 pool");
    require(gst_buffer_pool_set_active(self->pool, TRUE), "cannot activate RGB D3D11 pool");
}

static GstFlowReturn prepare_output_buffer(GstBaseTransform* transform, GstBuffer* input,
                                           GstBuffer** output) {
    auto* self = SELF(transform);
    try {
        ensure_output_pool(self, input);
        return gst_buffer_pool_acquire_buffer(self->pool, output, nullptr);
    } catch (const std::exception& error) {
        const HRESULT removed = device_removed_reason(self->device);
        if (FAILED(removed))
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("D3D11 device lost during RGB allocation"),
                              ("reason=%lu; %s; no fallback was attempted",
                               static_cast<unsigned long>(removed), error.what()));
        else
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("Native D3D11 RGB allocation failed"),
                              ("%s", error.what()));
        return GST_FLOW_ERROR;
    }
}

static GstFlowReturn transform(GstBaseTransform* transform, GstBuffer* input,
                               GstBuffer* output) {
    auto* self = SELF(transform);
    try {
        self->backend->convert(input, output);
        require(gst_buffer_copy_into(output, input,
                    static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_FLAGS |
                                                    GST_BUFFER_COPY_TIMESTAMPS), 0, 0),
                "cannot copy RGB buffer timestamps");
        return GST_FLOW_OK;
    } catch (const std::exception& error) {
        const HRESULT removed = device_removed_reason(self->device);
        if (FAILED(removed))
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("D3D11 device lost during RGB conversion"),
                              ("reason=%lu; %s; no fallback was attempted",
                               static_cast<unsigned long>(removed), error.what()));
        else
            GST_ELEMENT_ERROR(self, STREAM, DECODE, ("Native D3D11 RGB conversion failed"),
                              ("%s; no fallback was attempted", error.what()));
        return GST_FLOW_ERROR;
    }
}

static gboolean stop(GstBaseTransform* transform) {
    reset_gpu(SELF(transform));
    return TRUE;
}

static void finalize(GObject* object) {
    auto* self = SELF(object);
    reset_gpu(self);
    if (self->output_caps) gst_caps_unref(self->output_caps);
    G_OBJECT_CLASS(gst_prores_d3d11_rgb_parent_class)->finalize(object);
}

static void gst_prores_d3d11_rgb_class_init(GstProresD3D11RgbClass* klass) {
    auto* object = G_OBJECT_CLASS(klass);
    object->finalize = finalize;
    auto* element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Native D3D11 BT.709 RGB converter",
        "Filter/Converter/Video/Hardware", "planar ProRes 422/444 10/12-bit to RGB10A2_LE D3D11Memory without image readback",
        "ProRes GPU project");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
    auto* base = GST_BASE_TRANSFORM_CLASS(klass);
    base->transform_caps = transform_caps;
    base->set_caps = set_caps;
    base->prepare_output_buffer = prepare_output_buffer;
    base->transform = transform;
    base->stop = stop;
}

static void gst_prores_d3d11_rgb_init(GstProresD3D11Rgb* self) {
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), FALSE);
}

gboolean gst_prores_d3d11_rgb_register(GstPlugin* plugin) {
    return gst_element_register(plugin, "proresd3d11rgb", GST_RANK_NONE,
                                gst_prores_d3d11_rgb_get_type());
}
