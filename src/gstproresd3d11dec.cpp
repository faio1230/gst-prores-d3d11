/*
 * Minimal native D3D11 ProRes 422 HQ decoder for GStreamer.
 * Entropy and transform code is implemented by the companion SM5 shaders;
 * there is no libavcodec/Vulkan/software decode fallback in this module.
 */
#include "prores_parser.hpp"
#include "d3d11_hardware_device.hpp"
#include "d3d11_bounded_map.hpp"

#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>
#include <gst/d3d11/gstd3d11bufferpool.h>
#include <gst/d3d11/gstd3d11device.h>
#include <gst/d3d11/gstd3d11memory.h>
#include <gst/d3d11/gstd3d11utils.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

GST_DEBUG_CATEGORY_STATIC(proresd3d11_debug);
#define GST_CAT_DEFAULT proresd3d11_debug

namespace {

class UnsupportedDevice : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void check_hr(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " HRESULT=" +
                                 std::to_string(static_cast<unsigned long>(result)));
}

ComPtr<ID3D11Buffer> make_buffer(ID3D11Device* device, UINT bytes,
                                 UINT bind_flags, UINT misc_flags = 0,
                                 UINT stride = 0,
                                 D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                                 UINT cpu_access = 0) {
    if (!bytes) throw std::runtime_error("invalid D3D11 buffer size");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = usage;
    desc.BindFlags = bind_flags;
    desc.MiscFlags = misc_flags;
    desc.StructureByteStride = stride;
    desc.CPUAccessFlags = cpu_access;
    ComPtr<ID3D11Buffer> buffer;
    check_hr(device->CreateBuffer(&desc, nullptr, &buffer), "CreateBuffer");
    return buffer;
}

ComPtr<ID3D11Buffer> structured_buffer(ID3D11Device* device, UINT elements,
                                       UINT stride, UINT bind_flags,
                                       D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                                       UINT cpu_access = 0) {
    if (!elements || elements > UINT_MAX / stride)
        throw std::runtime_error("invalid structured D3D11 buffer size");
    return make_buffer(device, elements * stride, bind_flags,
        bind_flags ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0,
        bind_flags ? stride : 0, usage, cpu_access);
}

std::vector<std::uint8_t> read_shader(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open shader bytecode " + path.u8string());
    const auto size = stream.tellg();
    if (size <= 0 || size > 16 * 1024 * 1024)
        throw std::runtime_error("invalid shader bytecode size " + path.u8string());
    std::vector<std::uint8_t> bytecode(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytecode.data()), static_cast<std::streamsize>(size));
    if (!stream) throw std::runtime_error("cannot read shader bytecode " + path.u8string());
    return bytecode;
}

std::filesystem::path module_directory() {
    static int module_anchor;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_anchor), &module))
        throw std::runtime_error("cannot locate decoder module");
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
        throw std::runtime_error("cannot resolve decoder module path");
    return std::filesystem::path(path.data(), path.data() + length).parent_path();
}

struct VldParameters {
    std::uint32_t job_count;
    std::uint32_t coefficient_count;
    std::uint32_t reserved[2]{};
};

struct IdctParameters {
    std::uint32_t block_count;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t reserved = 0;
};

struct MemoryUav {
    ComPtr<ID3D11UnorderedAccessView> view;
};

struct GpuStageTiming {
    bool disjoint = false;
    double vld_ms = 0;
    double idct_ms = 0;
    double copy_ms = 0;
    double vld_to_copy_ms = 0;
};

struct CpuDecodeTiming {
    double coefficient_jobs_ms = 0;
    double idct_jobs_ms = 0;
    bool idct_layout_rebuilt = false;
    std::uint32_t idct_quant_slices_changed = 0;
    bool idct_gpu_upload = false;
    double cache_ms = 0;
    double output_uav_ms = 0;
    double device_lock_ms = 0;
    double upload_ms = 0;
    double vld_submit_ms = 0;
    double copy_ready_wait_ms = 0;
    double vld_map_ms = 0;
    std::uint64_t map_attempts = 0;
    double retire_wait_ms = 0;
    std::uint64_t retire_map_attempts = 0;
    double idct_submit_ms = 0;
};

ComPtr<ID3D11Query> make_query(ID3D11Device* device, D3D11_QUERY type) {
    D3D11_QUERY_DESC desc{};
    desc.Query = type;
    ComPtr<ID3D11Query> query;
    check_hr(device->CreateQuery(&desc, &query), "Create D3D11 query");
    return query;
}

struct GpuTimingQueries {
    explicit GpuTimingQueries(ID3D11Device* device)
        : disjoint(make_query(device, D3D11_QUERY_TIMESTAMP_DISJOINT)),
          vld_begin(make_query(device, D3D11_QUERY_TIMESTAMP)),
          vld_end(make_query(device, D3D11_QUERY_TIMESTAMP)),
          copy_begin(make_query(device, D3D11_QUERY_TIMESTAMP)),
          copy_end(make_query(device, D3D11_QUERY_TIMESTAMP)),
          idct_begin(make_query(device, D3D11_QUERY_TIMESTAMP)),
          idct_end(make_query(device, D3D11_QUERY_TIMESTAMP)) {}

    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> vld_begin;
    ComPtr<ID3D11Query> vld_end;
    ComPtr<ID3D11Query> copy_begin;
    ComPtr<ID3D11Query> copy_end;
    ComPtr<ID3D11Query> idct_begin;
    ComPtr<ID3D11Query> idct_end;
};

void destroy_memory_uav(gpointer data) {
    delete static_cast<MemoryUav*>(data);
}

class DeviceLock {
public:
    explicit DeviceLock(GstD3D11Device* device) : device_(device) {
        gst_d3d11_device_lock(device_);
    }
    ~DeviceLock() { gst_d3d11_device_unlock(device_); }
    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;
private:
    GstD3D11Device* device_;
};

class Dx11Backend {
public:
    Dx11Backend(GstD3D11Device* gst_device, const std::filesystem::path& shader_directory)
        : gst_device_(gst_device),
          device_(gst_d3d11_device_get_device_handle(gst_device)),
          context_(gst_d3d11_device_get_device_context_handle(gst_device)),
          token_(gst_d3d11_create_user_token()) {
        if (!device_ || !context_) throw std::runtime_error("GstD3D11Device has no native handles");
        try {
            require_d3d11_non_software_adapter(device_);
        } catch (const std::exception& error) {
            throw UnsupportedDevice(error.what());
        }
        const auto feature_level = device_->GetFeatureLevel();
        if (feature_level < D3D_FEATURE_LEVEL_11_0)
            throw UnsupportedDevice("ProRes SM5 requires D3D feature level 11_0 or higher; actual=" +
                                    std::to_string(static_cast<unsigned>(feature_level)));
        UINT format_support = 0;
        check_hr(device_->CheckFormatSupport(DXGI_FORMAT_R16_UNORM, &format_support),
                 "CheckFormatSupport R16_UNORM");
        constexpr UINT required_format_support = D3D11_FORMAT_SUPPORT_TEXTURE2D |
            D3D11_FORMAT_SUPPORT_SHADER_LOAD |
            D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
        if ((format_support & required_format_support) != required_format_support)
            throw UnsupportedDevice("R16_UNORM texture, shader load, or typed UAV unsupported; "
                                    "format support=" + std::to_string(format_support));
        const auto vld_bytecode = read_shader(shader_directory / L"prores_vld.cso");
        check_hr(device_->CreateComputeShader(vld_bytecode.data(), vld_bytecode.size(),
                                              nullptr, &vld_),
                 "Create VLD shader");
        const auto idct_bytecode = read_shader(shader_directory / L"prores_idct_unorm.cso");
        check_hr(device_->CreateComputeShader(idct_bytecode.data(), idct_bytecode.size(),
                                              nullptr, &idct_),
                 "Create IDCT shader");
        if (g_strcmp0(g_getenv("PRORES_DX11_GPU_TIMING"), "1") == 0)
            timing_ = std::make_unique<GpuTimingQueries>(device_);
    }

    ~Dx11Backend() { stop_error_worker(); }

    std::optional<GpuStageTiming> decode(const std::uint8_t* packet, std::size_t packet_size,
                                         const prores::Frame& parsed, GstBuffer* output,
                                         GstClockTime pts,
                                         CpuDecodeTiming* cpu_timing = nullptr) {
        auto phase_start = std::chrono::steady_clock::time_point{};
        if (cpu_timing) phase_start = std::chrono::steady_clock::now();
        const auto mark = [&](double CpuDecodeTiming::*field) {
            if (!cpu_timing) return;
            const auto now = std::chrono::steady_clock::now();
            cpu_timing->*field = std::chrono::duration<double, std::milli>(
                now - phase_start).count();
            phase_start = now;
        };
        check_hr(device_->GetDeviceRemovedReason(), "D3D11 device removed before decode");
        auto& coefficient_jobs = coefficient_jobs_;
        std::uint32_t coefficient_count = 0;
        prores::make_coefficient_jobs(parsed, coefficient_jobs, coefficient_count);
        mark(&CpuDecodeTiming::coefficient_jobs_ms);
        const auto idct_update = refresh_idct_jobs(parsed, coefficient_jobs);
        if (cpu_timing) {
            cpu_timing->idct_layout_rebuilt = idct_update.layout_rebuilt;
            cpu_timing->idct_quant_slices_changed = idct_update.quant_slices_changed;
        }
        auto& idct_jobs = idct_jobs_;
        mark(&CpuDecodeTiming::idct_jobs_ms);
        if (!coefficient_count || coefficient_jobs.empty() || idct_jobs.empty())
            throw std::runtime_error("empty ProRes GPU job list");

        if (packet_size > UINT_MAX - 3 || coefficient_jobs.size() > UINT_MAX ||
            idct_jobs.size() > UINT_MAX)
            throw std::runtime_error("frame exceeds D3D11 buffer addressing range");
        const auto cache_rebuilt = ensure_cache(parsed, static_cast<UINT>(packet_size),
                     static_cast<UINT>(coefficient_jobs.size()), coefficient_count,
                     static_cast<UINT>(idct_jobs.size()));
        const auto upload_idct_jobs = idct_update.dirty || cache_rebuilt;
        if (cpu_timing) cpu_timing->idct_gpu_upload = upload_idct_jobs;
        mark(&CpuDecodeTiming::cache_ms);

        if (gst_buffer_n_memory(output) != 3)
            throw std::runtime_error("I422_10LE D3D11 output must have three memories");
        ComPtr<ID3D11UnorderedAccessView> output_uavs[3];
        for (guint component = 0; component < 3; ++component) {
            GstMemory* memory = gst_buffer_peek_memory(output, component);
            if (!gst_is_d3d11_memory(memory))
                throw std::runtime_error("output pool returned non-D3D11 memory");
            auto* d3d_memory = GST_D3D11_MEMORY_CAST(memory);
            if (d3d_memory->device != gst_device_)
                throw std::runtime_error("output texture belongs to a different D3D11 device");
            D3D11_TEXTURE2D_DESC desc{};
            if (!gst_d3d11_memory_get_texture_desc(d3d_memory, &desc) ||
                desc.Format != DXGI_FORMAT_R16_UNORM || !(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ||
                desc.Width != static_cast<UINT>(component ? parsed.width / 2 : parsed.width) ||
                desc.Height != parsed.height)
                throw std::runtime_error("unexpected I422_10LE D3D11 texture layout");
            auto* stored = static_cast<MemoryUav*>(
                gst_d3d11_memory_get_token_data(d3d_memory, token_));
            if (!stored) {
                auto created = std::make_unique<MemoryUav>();
                check_hr(device_->CreateUnorderedAccessView(
                    gst_d3d11_memory_get_resource_handle(d3d_memory), nullptr, &created->view),
                    "Create output UAV");
                stored = created.get();
                gst_d3d11_memory_set_token_data(d3d_memory, token_, created.release(),
                                                destroy_memory_uav);
            }
            output_uavs[component] = stored->view;
        }
        mark(&CpuDecodeTiming::output_uav_ms);

        const auto slot_wait_start = std::chrono::steady_clock::now();
        wait_for_error_slot();
        if (cpu_timing)
            cpu_timing->retire_wait_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - slot_wait_start).count();

        std::optional<DeviceLock> lock(std::in_place, gst_device_);
        mark(&CpuDecodeTiming::device_lock_ms);
        D3D11_MAPPED_SUBRESOURCE mapped_packet{};
        check_hr(context_->Map(cache_.packet.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_packet),
                 "Map compressed packet upload");
        std::memcpy(mapped_packet.pData, packet, packet_size);
        if (packet_size & 3)
            std::memset(static_cast<std::uint8_t*>(mapped_packet.pData) + packet_size, 0,
                        4 - (packet_size & 3));
        context_->Unmap(cache_.packet.Get(), 0);
        const VldParameters vld_parameter_values{
            static_cast<std::uint32_t>(coefficient_jobs.size()), coefficient_count};
        const IdctParameters idct_parameter_values{
            static_cast<std::uint32_t>(idct_jobs.size()), parsed.width, parsed.height};
        std::vector<std::uint32_t> quant_matrices;
        quant_matrices.reserve(128);
        for (auto value : parsed.luma_quant_matrix) quant_matrices.push_back(value);
        for (auto value : parsed.chroma_quant_matrix) quant_matrices.push_back(value);
        context_->UpdateSubresource(cache_.coefficient_jobs.Get(), 0, nullptr,
                                    coefficient_jobs.data(), 0, 0);
        context_->UpdateSubresource(cache_.vld_parameters.Get(), 0, nullptr,
                                    &vld_parameter_values, 0, 0);
        if (upload_idct_jobs)
            context_->UpdateSubresource(cache_.idct_jobs.Get(), 0, nullptr,
                                        idct_jobs.data(), 0, 0);
        context_->UpdateSubresource(cache_.quant_matrices.Get(), 0, nullptr,
                                    quant_matrices.data(), 0, 0);
        context_->UpdateSubresource(cache_.idct_parameters.Get(), 0, nullptr,
                                    &idct_parameter_values, 0, 0);
        const UINT zeros[4]{};
        context_->ClearUnorderedAccessViewUint(cache_.coefficient_uav.Get(), zeros);
        context_->ClearUnorderedAccessViewUint(cache_.error_uav.Get(), zeros);
        mark(&CpuDecodeTiming::upload_ms);
        ID3D11ShaderResourceView* vld_srvs[] = {cache_.packet_srv.Get(), cache_.coefficient_job_srv.Get()};
        ID3D11UnorderedAccessView* vld_uavs[] = {cache_.coefficient_uav.Get(), cache_.error_uav.Get()};
        ID3D11Buffer* vld_constants[] = {cache_.vld_parameters.Get()};
        context_->CSSetShader(vld_.Get(), nullptr, 0);
        context_->CSSetShaderResources(0, 2, vld_srvs);
        context_->CSSetUnorderedAccessViews(0, 2, vld_uavs, nullptr);
        context_->CSSetConstantBuffers(0, 1, vld_constants);
        if (timing_) {
            context_->Begin(timing_->disjoint.Get());
            context_->End(timing_->vld_begin.Get());
        }
        context_->Dispatch(static_cast<UINT>((coefficient_jobs.size() + 63) / 64), 1, 1);
        if (timing_) context_->End(timing_->vld_end.Get());
        ID3D11UnorderedAccessView* null_vld_uavs[] = {nullptr, nullptr};
        ID3D11ShaderResourceView* null_vld_srvs[] = {nullptr, nullptr};
        context_->CSSetUnorderedAccessViews(0, 2, null_vld_uavs, nullptr);
        context_->CSSetShaderResources(0, 2, null_vld_srvs);
        mark(&CpuDecodeTiming::vld_submit_ms);

        ID3D11ShaderResourceView* idct_srvs[] = {
            cache_.coefficient_srv.Get(), cache_.idct_job_srv.Get(), cache_.quant_srv.Get()};
        ID3D11UnorderedAccessView* idct_uavs[] = {
            output_uavs[0].Get(), output_uavs[1].Get(), output_uavs[2].Get()};
        ID3D11Buffer* idct_constants[] = {cache_.idct_parameters.Get()};
        context_->CSSetShader(idct_.Get(), nullptr, 0);
        context_->CSSetShaderResources(0, 3, idct_srvs);
        context_->CSSetUnorderedAccessViews(0, 3, idct_uavs, nullptr);
        context_->CSSetConstantBuffers(0, 1, idct_constants);
        if (timing_) context_->End(timing_->idct_begin.Get());
        context_->Dispatch(static_cast<UINT>(idct_jobs.size()), 1, 1);
        if (timing_) context_->End(timing_->idct_end.Get());
        ID3D11UnorderedAccessView* null_idct_uavs[] = {nullptr, nullptr, nullptr};
        ID3D11ShaderResourceView* null_idct_srvs[] = {nullptr, nullptr, nullptr};
        context_->CSSetUnorderedAccessViews(0, 3, null_idct_uavs, nullptr);
        context_->CSSetShaderResources(0, 3, null_idct_srvs);
        context_->CSSetShader(nullptr, nullptr, 0);
        const auto staging = cache_.error_staging[next_staging_index_];
        const auto ready_query = cache_.error_ready[next_staging_index_];
        if (timing_) context_->End(timing_->copy_begin.Get());
        context_->CopyResource(staging.Get(), cache_.errors.Get());
        context_->End(ready_query.Get());
        if (timing_) {
            context_->End(timing_->copy_end.Get());
            context_->End(timing_->disjoint.Get());
        }
        // The pool can recycle these memory objects.  A direct UAV write does
        // not pass through GstD3D11Memory's map path, so invalidate its cached
        // staging copy explicitly before any downstream CPU mapping.
        for (guint component = 0; component < 3; ++component) {
            auto* memory = gst_buffer_peek_memory(output, component);
            GST_MEMORY_FLAG_UNSET(memory, GST_D3D11_MEMORY_TRANSFER_NEED_UPLOAD);
            GST_MINI_OBJECT_FLAG_SET(memory, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD);
        }
        mark(&CpuDecodeTiming::idct_submit_ms);
        lock.reset();
        {
            std::lock_guard<std::mutex> queue_lock(error_mutex_);
            if (!error_failure_.empty()) throw std::runtime_error(error_failure_);
            pending_errors_.push_back({staging, ready_query,
                                       static_cast<UINT>(coefficient_jobs.size()),
                                       frame_sequence_++, pts});
            next_staging_index_ = (next_staging_index_ + 1) % kErrorRingSize;
        }
        error_cv_.notify_all();
        if (timing_) return collect_timing();
        return std::nullopt;
    }

    void drain_errors() {
        std::unique_lock<std::mutex> queue_lock(error_mutex_);
        if (!error_cv_.wait_for(queue_lock, std::chrono::seconds(10), [&] {
                return pending_errors_.empty() || !error_failure_.empty();
            })) {
            queue_lock.unlock();
            check_hr(device_->GetDeviceRemovedReason(), "D3D11 device removed during VLD drain");
            throw std::runtime_error("delayed VLD error drain exceeded 10 seconds");
        }
        if (!error_failure_.empty()) throw std::runtime_error(error_failure_);
    }

    void discard_errors() {
        stop_error_worker();
        // flushing seekでは旧segmentの検査結果を新segmentへ持ち込まない。
        // GPU命令は同じimmediate context上で順序付きなので再利用先のcopyより先に完了する。
        {
            DeviceLock lock(gst_device_);
            context_->Flush();
        }
        {
            std::lock_guard<std::mutex> queue_lock(error_mutex_);
            pending_errors_.clear();
            error_failure_.clear();
        }
        next_staging_index_ = 0;
    }

private:
    static constexpr std::size_t kErrorRingSize = 3;

    struct PendingError {
        ComPtr<ID3D11Buffer> staging;
        ComPtr<ID3D11Query> ready;
        UINT job_count;
        std::uint64_t frame_sequence;
        GstClockTime pts;
    };

    bool inspect_error(const PendingError& pending, std::string& failure) {
        if (stop_error_worker_.load(std::memory_order_acquire)) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        {
            DeviceLock lock(gst_device_);
            context_->Flush();
        }
        for (;;) {
            if (stop_error_worker_.load(std::memory_order_acquire)) return false;
            HRESULT result;
            {
                DeviceLock lock(gst_device_);
                result = context_->GetData(pending.ready.Get(), nullptr, 0,
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
            }
            if (result == S_OK) break;
            if (result != S_FALSE) check_hr(result, "Get VLD error copy completion");
            check_hr(device_->GetDeviceRemovedReason(), "D3D11 device removed waiting for VLD error copy");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("delayed VLD error copy exceeded 10 seconds");
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        std::size_t failed_job = pending.job_count;
        for (;;) {
            if (stop_error_worker_.load(std::memory_order_acquire)) return false;
            HRESULT result;
            {
                DeviceLock lock(gst_device_);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                result = context_->Map(pending.staging.Get(), 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (result == S_OK) {
                    const auto* errors = static_cast<const std::uint32_t*>(mapped.pData);
                    for (std::size_t i = 0; i < pending.job_count; ++i) {
                        if (errors[i]) { failed_job = i; break; }
                    }
                    context_->Unmap(pending.staging.Get(), 0);
                }
            }
            if (result == S_OK) break;
            if (result != DXGI_ERROR_WAS_STILL_DRAWING)
                check_hr(result, "Map delayed VLD error flags");
            check_hr(device_->GetDeviceRemovedReason(), "D3D11 device removed while retiring VLD errors");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("delayed VLD error readback exceeded 10 seconds");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if (failed_job != pending.job_count)
            failure = "GPU entropy decoder rejected job " +
                std::to_string(failed_job) + " frame=" +
                std::to_string(pending.frame_sequence) + " pts_ns=" +
                std::to_string(pending.pts);
        return true;
    }

    void error_worker_loop() noexcept {
        for (;;) {
            PendingError pending;
            {
                std::unique_lock<std::mutex> queue_lock(error_mutex_);
                error_cv_.wait(queue_lock, [&] {
                    return stop_error_worker_.load(std::memory_order_acquire) ||
                           !pending_errors_.empty();
                });
                if (stop_error_worker_.load(std::memory_order_acquire)) return;
                pending = pending_errors_.front();
            }
            std::string failure;
            bool completed = false;
            try {
                completed = inspect_error(pending, failure);
            } catch (const std::exception& error) {
                failure = error.what();
                completed = true;
            } catch (...) {
                failure = "unknown failure while retiring VLD errors";
                completed = true;
            }
            const bool failed = !failure.empty();
            {
                std::lock_guard<std::mutex> queue_lock(error_mutex_);
                if (stop_error_worker_.load(std::memory_order_acquire)) return;
                if (!completed) return;
                pending_errors_.pop_front();
                if (!failure.empty()) error_failure_ = std::move(failure);
            }
            error_cv_.notify_all();
            if (failed) return;
        }
    }

    void start_error_worker() {
        if (error_worker_.joinable()) return;
        stop_error_worker_.store(false, std::memory_order_release);
        error_worker_ = std::thread([this] { error_worker_loop(); });
    }

    void stop_error_worker() {
        stop_error_worker_.store(true, std::memory_order_release);
        error_cv_.notify_all();
        if (error_worker_.joinable()) error_worker_.join();
    }

    void wait_for_error_slot() {
        start_error_worker();
        std::unique_lock<std::mutex> queue_lock(error_mutex_);
        if (!error_cv_.wait_for(queue_lock, std::chrono::seconds(10), [&] {
                return pending_errors_.size() < kErrorRingSize || !error_failure_.empty();
            })) {
            queue_lock.unlock();
            check_hr(device_->GetDeviceRemovedReason(), "D3D11 device removed waiting for VLD ring");
            throw std::runtime_error("delayed VLD error ring wait exceeded 10 seconds");
        }
        if (!error_failure_.empty()) throw std::runtime_error(error_failure_);
    }

    template <typename T>
    void wait_for_query(ID3D11Query* query, T& data) {
        {
            DeviceLock lock(gst_device_);
            context_->Flush();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            HRESULT result;
            {
                DeviceLock lock(gst_device_);
                result = context_->GetData(query, &data, sizeof(data), 0);
            }
            if (result == S_OK) return;
            check_hr(result, "Get GPU timing query");
            check_hr(device_->GetDeviceRemovedReason(), "D3D11 device removed during GPU timing");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("GPU timing query exceeded 10 seconds");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    GpuStageTiming collect_timing() {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        wait_for_query(timing_->disjoint.Get(), clock);
        if (clock.Disjoint) return {true, 0, 0, 0, 0};
        if (!clock.Frequency) throw std::runtime_error("GPU timestamp frequency is zero");
        UINT64 vld_begin = 0, vld_end = 0, copy_begin = 0, copy_end = 0;
        UINT64 idct_begin = 0, idct_end = 0;
        wait_for_query(timing_->vld_begin.Get(), vld_begin);
        wait_for_query(timing_->vld_end.Get(), vld_end);
        wait_for_query(timing_->copy_begin.Get(), copy_begin);
        wait_for_query(timing_->copy_end.Get(), copy_end);
        wait_for_query(timing_->idct_begin.Get(), idct_begin);
        wait_for_query(timing_->idct_end.Get(), idct_end);
        if (vld_begin > vld_end || vld_end > idct_begin || idct_begin > idct_end ||
            idct_end > copy_begin || copy_begin > copy_end)
            throw std::runtime_error("GPU timestamp order is invalid");
        const double scale = 1000.0 / static_cast<double>(clock.Frequency);
        return {false, (vld_end - vld_begin) * scale, (idct_end - idct_begin) * scale,
                (copy_end - copy_begin) * scale, (copy_end - vld_end) * scale};
    }

    struct Cache {
        UINT width = 0;
        UINT height = 0;
        UINT packet_capacity = 0;
        UINT coefficient_job_count = 0;
        UINT coefficient_count = 0;
        UINT idct_job_count = 0;
        ComPtr<ID3D11Buffer> packet;
        ComPtr<ID3D11Buffer> coefficient_jobs;
        ComPtr<ID3D11Buffer> coefficients;
        ComPtr<ID3D11Buffer> errors;
        std::array<ComPtr<ID3D11Buffer>, kErrorRingSize> error_staging;
        std::array<ComPtr<ID3D11Query>, kErrorRingSize> error_ready;
        ComPtr<ID3D11Buffer> vld_parameters;
        ComPtr<ID3D11Buffer> idct_jobs;
        ComPtr<ID3D11Buffer> quant_matrices;
        ComPtr<ID3D11Buffer> idct_parameters;
        ComPtr<ID3D11ShaderResourceView> packet_srv;
        ComPtr<ID3D11ShaderResourceView> coefficient_job_srv;
        ComPtr<ID3D11ShaderResourceView> coefficient_srv;
        ComPtr<ID3D11ShaderResourceView> idct_job_srv;
        ComPtr<ID3D11ShaderResourceView> quant_srv;
        ComPtr<ID3D11UnorderedAccessView> coefficient_uav;
        ComPtr<ID3D11UnorderedAccessView> error_uav;
    };

    struct IdctLayoutSlice {
        std::uint16_t mb_x = 0;
        std::uint16_t mb_y = 0;
        std::uint16_t mb_count = 0;
        std::uint32_t quant_scale = 0;
        std::size_t job_begin = 0;
        std::size_t job_end = 0;
    };

    struct IdctJobUpdate {
        bool dirty = false;
        bool layout_rebuilt = false;
        std::uint32_t quant_slices_changed = 0;
    };

    IdctJobUpdate refresh_idct_jobs(const prores::Frame& parsed,
                                    const std::vector<prores::CoefficientJob>& coefficient_jobs) {
        bool same_layout = idct_layout_width_ == parsed.width &&
                           idct_layout_height_ == parsed.height &&
                           idct_layout_.size() == parsed.slices.size() &&
                           coefficient_jobs.size() == parsed.slices.size() * 3;
        if (same_layout && (idct_layout_.empty() ||
                            idct_layout_.back().job_end != idct_jobs_.size()))
            same_layout = false;
        if (same_layout) {
            for (std::size_t i = 0; i < parsed.slices.size(); ++i) {
                const auto& slice = parsed.slices[i];
                const auto& layout = idct_layout_[i];
                if (layout.mb_x != slice.mb_x || layout.mb_y != slice.mb_y ||
                    layout.mb_count != slice.mb_count ||
                    layout.job_begin > layout.job_end ||
                    layout.job_end > idct_jobs_.size() ||
                    layout.job_end - layout.job_begin != static_cast<std::size_t>(slice.mb_count) * 8) {
                    same_layout = false;
                    break;
                }
            }
        }
        if (!same_layout) {
            prores::make_idct_jobs(parsed, coefficient_jobs, idct_jobs_);
            idct_layout_.clear();
            idct_layout_.reserve(parsed.slices.size());
            std::size_t job_begin = 0;
            for (const auto& slice : parsed.slices) {
                const auto quant_scale = slice.quant_index > 128
                    ? static_cast<std::uint32_t>(slice.quant_index - 96) * 4 : slice.quant_index;
                const auto job_end = job_begin + static_cast<std::size_t>(slice.mb_count) * 8;
                idct_layout_.push_back({slice.mb_x, slice.mb_y, slice.mb_count,
                                        quant_scale, job_begin, job_end});
                job_begin = job_end;
            }
            idct_layout_width_ = parsed.width;
            idct_layout_height_ = parsed.height;
            return {true, true, static_cast<std::uint32_t>(parsed.slices.size())};
        }
        IdctJobUpdate update{};
        for (std::size_t i = 0; i < parsed.slices.size(); ++i) {
            const auto quant_index = parsed.slices[i].quant_index;
            const auto quant_scale = quant_index > 128
                ? static_cast<std::uint32_t>(quant_index - 96) * 4 : quant_index;
            auto& layout = idct_layout_[i];
            if (layout.quant_scale == quant_scale) continue;
            for (auto job = layout.job_begin; job < layout.job_end; ++job)
                idct_jobs_[job].quant_scale = quant_scale;
            layout.quant_scale = quant_scale;
            update.dirty = true;
            ++update.quant_slices_changed;
        }
        return update;
    }

    bool ensure_cache(const prores::Frame& parsed, UINT packet_size,
                      UINT coefficient_job_count, UINT coefficient_count,
                      UINT idct_job_count) {
        const UINT padded_packet_size = (packet_size + 3u) & ~3u;
        if (cache_.width == parsed.width && cache_.height == parsed.height &&
            cache_.packet_capacity >= padded_packet_size &&
            cache_.coefficient_job_count == coefficient_job_count &&
            cache_.coefficient_count == coefficient_count &&
            cache_.idct_job_count == idct_job_count)
            return false;

        const std::uint64_t rounded =
            (static_cast<std::uint64_t>(padded_packet_size) + 65535u) & ~std::uint64_t(65535u);
        const UINT packet_capacity = rounded <= UINT_MAX
            ? static_cast<UINT>(rounded) : padded_packet_size;
        Cache replacement;
        replacement.width = parsed.width;
        replacement.height = parsed.height;
        replacement.packet_capacity = packet_capacity;
        replacement.coefficient_job_count = coefficient_job_count;
        replacement.coefficient_count = coefficient_count;
        replacement.idct_job_count = idct_job_count;

        replacement.packet = make_buffer(device_, packet_capacity,
            D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0,
            D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
        replacement.coefficient_jobs = structured_buffer(device_, coefficient_job_count,
            sizeof(prores::CoefficientJob), D3D11_BIND_SHADER_RESOURCE);
        replacement.coefficients = structured_buffer(device_, coefficient_count,
            sizeof(std::int32_t), D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        replacement.errors = structured_buffer(device_, coefficient_job_count,
            sizeof(std::uint32_t), D3D11_BIND_UNORDERED_ACCESS);
        for (auto& staging : replacement.error_staging)
            staging = structured_buffer(device_, coefficient_job_count,
                sizeof(std::uint32_t), 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        for (auto& ready : replacement.error_ready)
            ready = make_query(device_, D3D11_QUERY_EVENT);
        replacement.vld_parameters = make_buffer(device_, sizeof(VldParameters),
                                                   D3D11_BIND_CONSTANT_BUFFER);
        replacement.idct_jobs = structured_buffer(device_, idct_job_count,
            sizeof(prores::IdctBlockJob), D3D11_BIND_SHADER_RESOURCE);
        replacement.quant_matrices = structured_buffer(device_, 128,
            sizeof(std::uint32_t), D3D11_BIND_SHADER_RESOURCE);
        replacement.idct_parameters = make_buffer(device_, sizeof(IdctParameters),
                                                    D3D11_BIND_CONSTANT_BUFFER);

        D3D11_SHADER_RESOURCE_VIEW_DESC packet_view_desc{};
        packet_view_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        packet_view_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        packet_view_desc.BufferEx.NumElements = packet_capacity / 4;
        packet_view_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        check_hr(device_->CreateShaderResourceView(replacement.packet.Get(), &packet_view_desc,
                                                    &replacement.packet_srv),
                 "Create packet SRV");
        check_hr(device_->CreateShaderResourceView(replacement.coefficient_jobs.Get(), nullptr,
                                                    &replacement.coefficient_job_srv),
                 "Create coefficient job SRV");
        check_hr(device_->CreateShaderResourceView(replacement.coefficients.Get(), nullptr,
                                                    &replacement.coefficient_srv),
                 "Create coefficient SRV");
        check_hr(device_->CreateShaderResourceView(replacement.idct_jobs.Get(), nullptr,
                                                    &replacement.idct_job_srv),
                 "Create IDCT job SRV");
        check_hr(device_->CreateShaderResourceView(replacement.quant_matrices.Get(), nullptr,
                                                    &replacement.quant_srv),
                 "Create quant matrix SRV");
        check_hr(device_->CreateUnorderedAccessView(replacement.coefficients.Get(), nullptr,
                                                    &replacement.coefficient_uav),
                 "Create coefficient UAV");
        check_hr(device_->CreateUnorderedAccessView(replacement.errors.Get(), nullptr,
                                                    &replacement.error_uav),
                 "Create VLD error UAV");
        cache_ = std::move(replacement);
        next_staging_index_ = 0;
        return true;
    }

    GstD3D11Device* gst_device_;
    ID3D11Device* device_;
    ID3D11DeviceContext* context_;
    gint64 token_;
    ComPtr<ID3D11ComputeShader> vld_;
    ComPtr<ID3D11ComputeShader> idct_;
    std::unique_ptr<GpuTimingQueries> timing_;
    Cache cache_;
    std::mutex error_mutex_;
    std::condition_variable error_cv_;
    std::atomic<bool> stop_error_worker_{false};
    std::thread error_worker_;
    std::deque<PendingError> pending_errors_;
    std::string error_failure_;
    std::size_t next_staging_index_ = 0;
    std::uint64_t frame_sequence_ = 0;
    std::vector<prores::CoefficientJob> coefficient_jobs_;
    std::vector<prores::IdctBlockJob> idct_jobs_;
    std::vector<IdctLayoutSlice> idct_layout_;
    std::uint16_t idct_layout_width_ = 0;
    std::uint16_t idct_layout_height_ = 0;
};

}  // namespace

typedef struct _GstProresD3D11Dec {
    GstVideoDecoder parent;
    GstD3D11Device* device;
    GstVideoCodecState* input;
    Dx11Backend* backend;
    gchar* shader_directory;
    gint adapter;
    gboolean negotiated;
    gboolean failed;
    gint flushing;
    gint color_primaries;
    gint color_trc;
    gint color_matrix;
    gboolean cpu_timing;
    guint64 cpu_timing_sequence;
} GstProresD3D11Dec;

typedef struct _GstProresD3D11DecClass { GstVideoDecoderClass parent_class; } GstProresD3D11DecClass;
G_DEFINE_TYPE(GstProresD3D11Dec, gst_prores_d3d11_dec, GST_TYPE_VIDEO_DECODER)
#define SELF(obj) (reinterpret_cast<GstProresD3D11Dec*>(obj))

enum { PROP_0, PROP_ADAPTER, PROP_SHADER_DIRECTORY };

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-prores, variant=(string)hq, width=(int)[16,8192], "
                    "height=(int)[16,8192], interlace-mode=(string)progressive"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:D3D11Memory), format=(string)I422_10LE, "
                    "width=(int)[16,8192], height=(int)[16,8192], "
                    "interlace-mode=(string)progressive"));

static void clear_format(GstProresD3D11Dec* self) {
    if (self->input) gst_video_codec_state_unref(self->input);
    self->input = nullptr;
    self->negotiated = FALSE;
}

static gboolean start(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    self->failed = FALSE;
    self->negotiated = FALSE;
    self->cpu_timing = g_strcmp0(g_getenv("PRORES_DX11_CPU_TIMING"), "1") == 0;
    self->cpu_timing_sequence = 0;
    g_atomic_int_set(&self->flushing, 0);
    if (!gst_d3d11_ensure_element_data(GST_ELEMENT(self), self->adapter, &self->device)) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, ("Cannot create D3D11 device"),
                          ("adapter=%d", self->adapter));
        return FALSE;
    }
    try {
        const gchar* environment = g_getenv("PRORES_DX11_SHADER_DIR");
        const auto directory = environment && *environment
            ? std::filesystem::u8path(environment)
            : self->shader_directory && *self->shader_directory
                ? std::filesystem::u8path(self->shader_directory)
                : module_directory();
        delete self->backend;
        self->backend = new Dx11Backend(self->device, directory);
        GST_INFO_OBJECT(self, "native D3D11 shaders loaded from %s", directory.u8string().c_str());
    } catch (const UnsupportedDevice& error) {
        GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("D3D11 adapter lacks decoder capabilities"),
                          ("%s; no fallback was attempted", error.what()));
        return FALSE;
    } catch (const std::exception& error) {
        auto* native = self->device ? gst_d3d11_device_get_device_handle(self->device) : nullptr;
        const HRESULT removed = native ? native->GetDeviceRemovedReason() : S_OK;
        if (FAILED(removed))
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("D3D11 device lost during initialization"),
                              ("reason=%lu; %s; no fallback was attempted",
                               static_cast<unsigned long>(removed), error.what()));
        else
            GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, ("Cannot initialize native D3D11 decoder"),
                              ("%s", error.what()));
        return FALSE;
    }
    return TRUE;
}

static gboolean stop(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    if (self->backend) self->backend->discard_errors();
    delete self->backend;
    self->backend = nullptr;
    clear_format(self);
    if (self->device) gst_clear_object(&self->device);
    return TRUE;
}

static gboolean set_format(GstVideoDecoder* decoder, GstVideoCodecState* state) {
    auto* self = SELF(decoder);
    clear_format(self);
    const auto* structure = gst_caps_get_structure(state->caps, 0);
    const char* variant = gst_structure_get_string(structure, "variant");
    const char* interlace = gst_structure_get_string(structure, "interlace-mode");
    const int width = GST_VIDEO_INFO_WIDTH(&state->info);
    const int height = GST_VIDEO_INFO_HEIGHT(&state->info);
    if (g_strcmp0(variant, "hq") != 0 ||
        (interlace && g_strcmp0(interlace, "progressive") != 0) ||
        width < 16 || width > 8192 || height < 16 || height > 8192 || (width & 1)) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT,
            ("Only progressive, alpha-free ProRes 422 HQ 10-bit is supported"),
            ("caps: %" GST_PTR_FORMAT, state->caps));
        return FALSE;
    }
    self->input = gst_video_codec_state_ref(state);
    return TRUE;
}

static gboolean negotiate_output(GstProresD3D11Dec* self, const prores::Frame& parsed) {
    if (self->negotiated && self->color_primaries == parsed.color_primaries &&
        self->color_trc == parsed.transfer_characteristic &&
        self->color_matrix == parsed.matrix_coefficients) return TRUE;
    auto* decoder = GST_VIDEO_DECODER(self);
    auto* state = gst_video_decoder_set_output_state(decoder, GST_VIDEO_FORMAT_I422_10LE,
                                                      parsed.width, parsed.height, self->input);
    if (!state) return FALSE;
    state->info.interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
    state->info.colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
    state->info.colorimetry.matrix = gst_video_color_matrix_from_iso(parsed.matrix_coefficients);
    state->info.colorimetry.primaries = gst_video_color_primaries_from_iso(parsed.color_primaries);
    state->info.colorimetry.transfer = gst_video_transfer_function_from_iso(parsed.transfer_characteristic);
    GstVideoColorimetry upstream{};
    const char* color = gst_structure_get_string(gst_caps_get_structure(self->input->caps, 0),
                                                  "colorimetry");
    if (color && gst_video_colorimetry_from_string(&upstream, color)) {
        if (state->info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN)
            state->info.colorimetry.matrix = upstream.matrix;
        if (state->info.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN)
            state->info.colorimetry.primaries = upstream.primaries;
        if (state->info.colorimetry.transfer == GST_VIDEO_TRANSFER_UNKNOWN)
            state->info.colorimetry.transfer = upstream.transfer;
    }
    if (state->caps) gst_caps_unref(state->caps);
    state->caps = gst_video_info_to_caps(&state->info);
    gst_caps_set_features(state->caps, 0,
        gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY, nullptr));
    gst_video_codec_state_unref(state);
    if (!gst_video_decoder_negotiate(decoder)) return FALSE;
    self->color_primaries = parsed.color_primaries;
    self->color_trc = parsed.transfer_characteristic;
    self->color_matrix = parsed.matrix_coefficients;
    self->negotiated = TRUE;
    return TRUE;
}

static gboolean decide_allocation(GstVideoDecoder* decoder, GstQuery* query) {
    auto* self = SELF(decoder);
    GstCaps* caps = nullptr;
    gst_query_parse_allocation(query, &caps, nullptr);
    if (!caps || !gst_caps_features_contains(gst_caps_get_features(caps, 0),
                                             GST_CAPS_FEATURE_MEMORY_D3D11_MEMORY))
        return FALSE;
    GstVideoInfo info{};
    if (!gst_video_info_from_caps(&info, caps)) return FALSE;
    if (info.size > G_MAXUINT) return FALSE;
    const auto buffer_size = static_cast<guint>(info.size);
    GstBufferPool* pool = gst_d3d11_buffer_pool_new(self->device);
    if (!pool) return FALSE;
    GstStructure* config = gst_buffer_pool_get_config(pool);
    gst_buffer_pool_config_set_params(config, caps, buffer_size, 3, 0);
    gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
    auto* params = gst_d3d11_allocation_params_new(self->device, &info,
        GST_D3D11_ALLOCATION_FLAG_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0);
    if (!params) { gst_object_unref(pool); return FALSE; }
    gst_buffer_pool_config_set_d3d11_allocation_params(config, params);
    gst_d3d11_allocation_params_free(params);
    if (!gst_buffer_pool_set_config(pool, config)) { gst_object_unref(pool); return FALSE; }
    if (gst_query_get_n_allocation_pools(query))
        gst_query_set_nth_allocation_pool(query, 0, pool, buffer_size, 3, 0);
    else
        gst_query_add_allocation_pool(query, pool, buffer_size, 3, 0);
    gst_object_unref(pool);
    return GST_VIDEO_DECODER_CLASS(gst_prores_d3d11_dec_parent_class)->decide_allocation(decoder, query);
}

static GstFlowReturn handle_frame(GstVideoDecoder* decoder, GstVideoCodecFrame* frame) {
    auto* self = SELF(decoder);
    const auto cpu_timing = self->cpu_timing;
    const auto sequence = cpu_timing ? self->cpu_timing_sequence++ : 0;
    auto phase_start = std::chrono::steady_clock::time_point{};
    if (cpu_timing) phase_start = std::chrono::steady_clock::now();
    const auto phase_ms = [&] {
        if (!cpu_timing) return 0.0;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double, std::milli>(
            now - phase_start).count();
        phase_start = now;
        return elapsed;
    };
    if (self->failed || !self->backend || !self->input) {
        gst_video_decoder_drop_frame(decoder, frame);
        return GST_FLOW_ERROR;
    }
    if (g_atomic_int_get(&self->flushing)) {
        gst_video_decoder_drop_frame(decoder, frame);
        return GST_FLOW_FLUSHING;
    }
    GstMapInfo input{};
    if (!gst_buffer_map(frame->input_buffer, &input, GST_MAP_READ)) {
        gst_video_decoder_drop_frame(decoder, frame);
        return GST_FLOW_ERROR;
    }
    prores::Frame parsed;
    std::string error;
    const auto expected_width = static_cast<std::uint16_t>(GST_VIDEO_INFO_WIDTH(&self->input->info));
    const auto expected_height = static_cast<std::uint16_t>(GST_VIDEO_INFO_HEIGHT(&self->input->info));
    if (!prores::parse_frame(input.data, input.size, expected_width, expected_height, parsed, error)) {
        gst_buffer_unmap(frame->input_buffer, &input);
        self->failed = TRUE;
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Malformed or unsupported ProRes frame"),
                          ("%s", error.c_str()));
        gst_video_decoder_drop_frame(decoder, frame);
        return GST_FLOW_ERROR;
    }
    const auto parse_ms = phase_ms();
    if (!negotiate_output(self, parsed)) {
        gst_buffer_unmap(frame->input_buffer, &input);
        gst_video_decoder_drop_frame(decoder, frame);
        return GST_FLOW_NOT_NEGOTIATED;
    }
    const auto negotiate_ms = phase_ms();
    auto flow = gst_video_decoder_allocate_output_frame(decoder, frame);
    if (flow != GST_FLOW_OK) {
        gst_buffer_unmap(frame->input_buffer, &input);
        gst_video_decoder_drop_frame(decoder, frame);
        return flow;
    }
    const auto allocate_ms = phase_ms();
    CpuDecodeTiming decode_timing{};
    try {
        const auto timing = self->backend->decode(input.data, input.size, parsed,
                                                  frame->output_buffer,
                                                  GST_BUFFER_PTS(frame->input_buffer),
                                                  cpu_timing ? &decode_timing : nullptr);
        if (timing) {
            const auto pts = GST_BUFFER_PTS(frame->input_buffer);
            if (timing->disjoint)
                GST_INFO_OBJECT(self, "GPU_STAGE_DISJOINT pts_ns=%" G_GUINT64_FORMAT, pts);
            else
                GST_INFO_OBJECT(self, "GPU_STAGE pts_ns=%" G_GUINT64_FORMAT
                                " vld_ms=%.6f idct_ms=%.6f copy_ms=%.6f"
                                " vld_to_copy_ms=%.6f", pts,
                                timing->vld_ms, timing->idct_ms,
                                timing->copy_ms, timing->vld_to_copy_ms);
        }
    } catch (const std::exception& exception) {
        gst_buffer_unmap(frame->input_buffer, &input);
        self->failed = TRUE;
        auto* native = self->device ? gst_d3d11_device_get_device_handle(self->device) : nullptr;
        const HRESULT removed = native ? native->GetDeviceRemovedReason() : S_OK;
        if (FAILED(removed))
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("D3D11 device lost during ProRes decode"),
                              ("reason=%lu; %s; no fallback was attempted",
                               static_cast<unsigned long>(removed), exception.what()));
        else
            GST_ELEMENT_ERROR(self, STREAM, DECODE, ("Native D3D11 ProRes decode failed"),
                              ("%s; no fallback was attempted", exception.what()));
        gst_video_decoder_drop_frame(decoder, frame);
        return GST_FLOW_ERROR;
    }
    gst_buffer_unmap(frame->input_buffer, &input);
    const auto backend_ms = phase_ms();
    const auto pts = GST_BUFFER_PTS(frame->input_buffer);
    GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT(frame);
    flow = gst_video_decoder_finish_frame(decoder, frame);
    const auto finish_ms = phase_ms();
    if (cpu_timing)
        GST_INFO_OBJECT(self, "CPU_STAGE seq=%" G_GUINT64_FORMAT
                        " pts_ns=%" G_GUINT64_FORMAT
                        " parse_ms=%.6f negotiate_ms=%.6f allocate_ms=%.6f"
                        " coefficient_jobs_ms=%.6f idct_jobs_ms=%.6f"
                        " idct_layout_rebuilt=%u idct_quant_slices_changed=%u"
                        " idct_gpu_upload=%u cache_ms=%.6f"
                        " output_uav_ms=%.6f device_lock_ms=%.6f"
                        " upload_ms=%.6f vld_submit_ms=%.6f"
                        " copy_ready_wait_ms=%.6f vld_map_ms=%.6f"
                        " map_attempts=%" G_GUINT64_FORMAT " idct_submit_ms=%.6f"
                        " retire_wait_ms=%.6f retire_map_attempts=%" G_GUINT64_FORMAT
                        " backend_ms=%.6f finish_ms=%.6f", sequence, pts,
                        parse_ms, negotiate_ms, allocate_ms,
                        decode_timing.coefficient_jobs_ms, decode_timing.idct_jobs_ms,
                        decode_timing.idct_layout_rebuilt,
                        decode_timing.idct_quant_slices_changed,
                        decode_timing.idct_gpu_upload,
                        decode_timing.cache_ms, decode_timing.output_uav_ms,
                        decode_timing.device_lock_ms, decode_timing.upload_ms,
                        decode_timing.vld_submit_ms, decode_timing.copy_ready_wait_ms,
                        decode_timing.vld_map_ms,
                        decode_timing.map_attempts, decode_timing.idct_submit_ms,
                        decode_timing.retire_wait_ms, decode_timing.retire_map_attempts,
                        backend_ms, finish_ms);
    return flow;
}

static gboolean flush(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    if (self->backend) self->backend->discard_errors();
    return TRUE;
}

static GstFlowReturn finish(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    if (self->failed || !self->backend) return GST_FLOW_ERROR;
    try {
        self->backend->drain_errors();
        return GST_FLOW_OK;
    } catch (const std::exception& error) {
        self->failed = TRUE;
        auto* native = self->device ? gst_d3d11_device_get_device_handle(self->device) : nullptr;
        const HRESULT removed = native ? native->GetDeviceRemovedReason() : S_OK;
        if (FAILED(removed))
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("D3D11 device lost during ProRes drain"),
                              ("reason=%lu; %s; no fallback was attempted",
                               static_cast<unsigned long>(removed), error.what()));
        else
            GST_ELEMENT_ERROR(self, STREAM, DECODE, ("Native D3D11 ProRes decode failed"),
                              ("%s; no fallback was attempted", error.what()));
        return GST_FLOW_ERROR;
    }
}

static gboolean sink_event(GstVideoDecoder* decoder, GstEvent* event) {
    auto* self = SELF(decoder);
    const auto type = GST_EVENT_TYPE(event);
    if (type == GST_EVENT_FLUSH_START) g_atomic_int_set(&self->flushing, 1);
    const auto result = GST_VIDEO_DECODER_CLASS(gst_prores_d3d11_dec_parent_class)->sink_event(decoder, event);
    if (type == GST_EVENT_FLUSH_STOP) {
        self->failed = FALSE;
        g_atomic_int_set(&self->flushing, 0);
    }
    return result;
}

static gboolean src_query(GstVideoDecoder* decoder, GstQuery* query) {
    auto* self = SELF(decoder);
    if (GST_QUERY_TYPE(query) == GST_QUERY_CONTEXT && self->device &&
        gst_d3d11_handle_context_query(GST_ELEMENT(self), query, self->device))
        return TRUE;
    return GST_VIDEO_DECODER_CLASS(gst_prores_d3d11_dec_parent_class)->src_query(decoder, query);
}

static gboolean sink_query(GstVideoDecoder* decoder, GstQuery* query) {
    auto* self = SELF(decoder);
    if (GST_QUERY_TYPE(query) == GST_QUERY_CONTEXT && self->device &&
        gst_d3d11_handle_context_query(GST_ELEMENT(self), query, self->device))
        return TRUE;
    return GST_VIDEO_DECODER_CLASS(gst_prores_d3d11_dec_parent_class)->sink_query(decoder, query);
}

static void set_context(GstElement* element, GstContext* context) {
    auto* self = SELF(element);
    gst_d3d11_handle_set_context(element, context, self->adapter, &self->device);
    GST_ELEMENT_CLASS(gst_prores_d3d11_dec_parent_class)->set_context(element, context);
}

static void set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec) {
    auto* self = SELF(object);
    switch (id) {
        case PROP_ADAPTER: self->adapter = g_value_get_int(value); break;
        case PROP_SHADER_DIRECTORY:
            g_free(self->shader_directory);
            self->shader_directory = g_value_dup_string(value);
            break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
    }
}

static void get_property(GObject* object, guint id, GValue* value, GParamSpec* spec) {
    auto* self = SELF(object);
    switch (id) {
        case PROP_ADAPTER: g_value_set_int(value, self->adapter); break;
        case PROP_SHADER_DIRECTORY: g_value_set_string(value, self->shader_directory); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
    }
}

static void finalize(GObject* object) {
    auto* self = SELF(object);
    if (self->backend) self->backend->discard_errors();
    delete self->backend;
    clear_format(self);
    if (self->device) gst_clear_object(&self->device);
    g_free(self->shader_directory);
    G_OBJECT_CLASS(gst_prores_d3d11_dec_parent_class)->finalize(object);
}

static void gst_prores_d3d11_dec_class_init(GstProresD3D11DecClass* klass) {
    auto* object = G_OBJECT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    const auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(object, PROP_ADAPTER,
        g_param_spec_int("adapter", "D3D11 adapter", "DXGI adapter index (-1 selects default)",
                         -1, G_MAXINT, -1, flags));
    g_object_class_install_property(object, PROP_SHADER_DIRECTORY,
        g_param_spec_string("shader-directory", "Shader directory",
            "Override directory containing prores_vld.cso and prores_idct_unorm.cso", nullptr, flags));
    auto* element = GST_ELEMENT_CLASS(klass);
    element->set_context = set_context;
    gst_element_class_set_static_metadata(element, "Native D3D11 ProRes decoder",
        "Codec/Decoder/Video/Hardware", "Progressive ProRes 422 HQ to D3D11Memory without image readback",
        "ProRes GPU project");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
    auto* decoder = GST_VIDEO_DECODER_CLASS(klass);
    decoder->start = start;
    decoder->stop = stop;
    decoder->set_format = set_format;
    decoder->handle_frame = handle_frame;
    decoder->decide_allocation = decide_allocation;
    decoder->flush = flush;
    decoder->finish = finish;
    decoder->drain = finish;
    decoder->sink_event = sink_event;
    decoder->src_query = src_query;
    decoder->sink_query = sink_query;
}

static void gst_prores_d3d11_dec_init(GstProresD3D11Dec* self) {
    self->adapter = -1;
    self->shader_directory = nullptr;
    gst_video_decoder_set_packetized(GST_VIDEO_DECODER(self), TRUE);
    gst_video_decoder_set_needs_format(GST_VIDEO_DECODER(self), TRUE);
}

static gboolean plugin_init(GstPlugin* plugin) {
    extern gboolean gst_prores_d3d11_rgb_register(GstPlugin* plugin);
    GST_DEBUG_CATEGORY_INIT(proresd3d11_debug, "proresd3d11dec", 0,
                            "Native D3D11 ProRes decoder");
    return gst_element_register(plugin, "proresd3d11dec", GST_RANK_NONE,
                                gst_prores_d3d11_dec_get_type()) &&
           gst_prores_d3d11_rgb_register(plugin);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, proresd3d11,
    "Native D3D11 ProRes 422 HQ decoder and RGB converter", plugin_init, "0.1.0", "LGPL",
    "prores-gpu-lab", "https://example.invalid/prores-gpu-lab")
