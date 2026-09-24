// Actual-frame CPU/D3D11 comparison for ProRes VLD and inverse scan.
#include "prores_parser.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " HRESULT=" +
                                 std::to_string(static_cast<unsigned long>(result)));
}

struct Packet {
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
    std::uint32_t codec_tag = 0;
};

Packet read_video_packet(const char* path, std::size_t target_index) {
    AVFormatContext* raw_format = nullptr;
    int result = avformat_open_input(&raw_format, path, nullptr, nullptr);
    if (result < 0) throw std::runtime_error("avformat_open_input failed");
    struct FormatCloser {
        void operator()(AVFormatContext* value) const { avformat_close_input(&value); }
    };
    std::unique_ptr<AVFormatContext, FormatCloser> format(raw_format);
    if (avformat_find_stream_info(format.get(), nullptr) < 0)
        throw std::runtime_error("avformat_find_stream_info failed");
    const int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) throw std::runtime_error("video stream missing");
    const auto* parameters = format->streams[stream]->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_PRORES)
        throw std::runtime_error("video stream is not ProRes");

    AVPacket* raw_packet = av_packet_alloc();
    if (!raw_packet) throw std::bad_alloc();
    struct PacketCloser {
        void operator()(AVPacket* value) const { av_packet_free(&value); }
    };
    std::unique_ptr<AVPacket, PacketCloser> packet(raw_packet);
    std::size_t video_index = 0;
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == stream) {
            if (video_index++ == target_index) {
                Packet output;
                output.bytes.assign(packet->data, packet->data + packet->size);
                output.width = parameters->width;
                output.height = parameters->height;
                output.codec_tag = parameters->codec_tag;
                return output;
            }
        }
        av_packet_unref(packet.get());
    }
    throw std::runtime_error("video packet index missing");
}

struct CpuFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> planes[3];
};

CpuFrame decode_cpu(const Packet& packet) {
    const auto* decoder = avcodec_find_decoder(AV_CODEC_ID_PRORES);
    if (!decoder) throw std::runtime_error("CPU ProRes decoder missing");
    AVCodecContext* raw_context = avcodec_alloc_context3(decoder);
    if (!raw_context) throw std::bad_alloc();
    struct ContextCloser {
        void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
    };
    std::unique_ptr<AVCodecContext, ContextCloser> context(raw_context);
    context->width = packet.width;
    context->height = packet.height;
    context->codec_tag = packet.codec_tag;
    context->thread_count = 1;
    context->err_recognition = AV_EF_EXPLODE;
    if (avcodec_open2(context.get(), decoder, nullptr) < 0)
        throw std::runtime_error("avcodec_open2 CPU reference failed");
    AVPacket* raw_packet = av_packet_alloc();
    AVFrame* raw_frame = av_frame_alloc();
    if (!raw_packet || !raw_frame) {
        av_packet_free(&raw_packet);
        av_frame_free(&raw_frame);
        throw std::bad_alloc();
    }
    struct DecodeObjects {
        AVPacket* packet;
        AVFrame* frame;
        ~DecodeObjects() { av_packet_free(&packet); av_frame_free(&frame); }
    } objects{raw_packet, raw_frame};
    if (av_new_packet(objects.packet, static_cast<int>(packet.bytes.size())) < 0)
        throw std::bad_alloc();
    std::memcpy(objects.packet->data, packet.bytes.data(), packet.bytes.size());
    if (avcodec_send_packet(context.get(), objects.packet) < 0 ||
        avcodec_receive_frame(context.get(), objects.frame) < 0)
        throw std::runtime_error("CPU ProRes decode failed");
    if (objects.frame->format != AV_PIX_FMT_YUV422P10LE)
        throw std::runtime_error("unexpected CPU reference format");
    CpuFrame output;
    output.width = objects.frame->width;
    output.height = objects.frame->height;
    for (int component = 0; component < 3; ++component) {
        const int width = component ? output.width / 2 : output.width;
        output.planes[component].resize(static_cast<std::size_t>(width) * output.height);
        for (int y = 0; y < output.height; ++y)
            std::memcpy(output.planes[component].data() + static_cast<std::size_t>(y) * width,
                        objects.frame->data[component] + static_cast<std::size_t>(y) * objects.frame->linesize[component],
                        static_cast<std::size_t>(width) * sizeof(std::uint16_t));
    }
    return output;
}

template <typename T>
ComPtr<ID3D11Buffer> immutable_buffer(ID3D11Device* device, const std::vector<T>& values,
                                      UINT bind_flags, UINT misc_flags = 0,
                                      UINT stride = 0) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(values.size() * sizeof(T));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = bind_flags;
    desc.MiscFlags = misc_flags;
    desc.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA initial{values.data(), 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, &initial, &buffer), "CreateBuffer immutable");
    return buffer;
}

ComPtr<ID3D11Buffer> output_buffer(ID3D11Device* device, UINT bytes, UINT stride) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, &buffer), "CreateBuffer output");
    return buffer;
}

ComPtr<ID3D11Texture2D> output_texture(ID3D11Device* device, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D output");
    return texture;
}

ComPtr<ID3D11Texture2D> staging_texture(ID3D11Device* device, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D staging");
    return texture;
}

ComPtr<ID3DBlob> compile_shader(const wchar_t* path) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> diagnostics;
    const auto result = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &bytecode, &diagnostics);
    if (FAILED(result) && diagnostics)
        std::cerr.write(static_cast<const char*>(diagnostics->GetBufferPointer()),
                        diagnostics->GetBufferSize());
    check(result, "D3DCompileFromFile");
    return bytecode;
}

ComPtr<ID3DBlob> load_or_compile_shader(const wchar_t* path) {
    if (std::filesystem::path(path).extension() != L".cso")
        return compile_shader(path);
    ComPtr<ID3DBlob> bytecode;
    check(D3DReadFileToBlob(path, &bytecode), "D3DReadFileToBlob");
    return bytecode;
}

ComPtr<ID3D11Buffer> staging_buffer(ID3D11Device* device, UINT bytes) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, &buffer), "CreateBuffer staging");
    return buffer;
}

struct Parameters {
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

struct PixelDifference {
    std::uint64_t count = 0;
    std::uint32_t maximum = 0;
    std::uint64_t absolute_sum = 0;
    std::uint16_t cpu_minimum = 1023;
    std::uint16_t cpu_maximum = 0;
    std::uint16_t gpu_minimum = 1023;
    std::uint16_t gpu_maximum = 0;
    std::uint16_t first_cpu = 0;
    std::uint16_t first_gpu = 0;
};

std::uint16_t read_be16(const std::uint8_t* value) {
    return static_cast<std::uint16_t>((value[0] << 8) | value[1]);
}

void write_be32(std::uint8_t* value, std::uint32_t number) {
    value[0] = static_cast<std::uint8_t>(number >> 24);
    value[1] = static_cast<std::uint8_t>(number >> 16);
    value[2] = static_cast<std::uint8_t>(number >> 8);
    value[3] = static_cast<std::uint8_t>(number);
}

std::size_t verify_parser_rejections(const Packet& packet, const prores::Frame& parsed) {
    std::size_t rejected = 0;
    auto expect_rejection = [&](std::vector<std::uint8_t> bytes,
                                std::uint16_t width, std::uint16_t height,
                                const char* name) {
        prores::Frame ignored;
        std::string error;
        if (prores::parse_frame(bytes.data(), bytes.size(), width, height, ignored, error))
            throw std::runtime_error(std::string("parser accepted malformed case: ") + name);
        ++rejected;
    };
    auto mutation = packet.bytes;
    mutation[4] = 'x';
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "signature");
    mutation = packet.bytes;
    mutation[0] ^= 1;
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "frame-size");
    mutation = packet.bytes;
    mutation[20] = static_cast<std::uint8_t>((mutation[20] & ~0xc0) | 0xc0);
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "4444");
    mutation = packet.bytes;
    mutation[20] |= 4;
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "interlace");
    mutation = packet.bytes;
    mutation[25] |= 1;
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "alpha");
    mutation = packet.bytes;
    const std::size_t picture_offset = 8 + read_be16(mutation.data() + 8);
    mutation[picture_offset + 1] = 0xff;
    mutation[picture_offset + 2] = 0xff;
    mutation[picture_offset + 3] = 0xff;
    mutation[picture_offset + 4] = 0xff;
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "picture-size");
    mutation = packet.bytes;
    const std::size_t index_offset = picture_offset + (mutation[picture_offset] >> 3);
    mutation[index_offset] = 0xff;
    mutation[index_offset + 1] = 0xff;
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "slice-size");
    mutation = packet.bytes;
    mutation[parsed.slices[0].offset + 2] = 0xff;
    mutation[parsed.slices[0].offset + 3] = 0xff;
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "plane-size");
    mutation = packet.bytes;
    mutation.resize(parsed.slices.back().offset + parsed.slices.back().size);
    mutation.insert(mutation.end(), 16 - (mutation.size() & 15), 0);
    write_be32(mutation.data(), static_cast<std::uint32_t>(mutation.size()));
    prores::Frame zero_padded;
    std::string padding_error;
    if (!prores::parse_frame(mutation.data(), mutation.size(), parsed.width,
                             parsed.height, zero_padded, padding_error))
        throw std::runtime_error("zero-padded frame rejected: " + padding_error);
    mutation = packet.bytes;
    mutation.push_back(0x7f);
    if ((mutation.size() & 511) == 0) mutation.push_back(0x7f);
    write_be32(mutation.data(), static_cast<std::uint32_t>(mutation.size()));
    write_be32(mutation.data() + picture_offset + 1,
               static_cast<std::uint32_t>(mutation.size() - picture_offset));
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "unindexed-picture-tail");
    mutation = packet.bytes;
    mutation.push_back(0x7f);
    if ((mutation.size() & 511) == 0) mutation.push_back(0x7f);
    write_be32(mutation.data(), static_cast<std::uint32_t>(mutation.size()));
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "unaligned-frame-trailer");
    mutation = packet.bytes;
    mutation.insert(mutation.end(),
                    512 + ((512 - (mutation.size() & 511)) & 511), 0);
    write_be32(mutation.data(), static_cast<std::uint32_t>(mutation.size()));
    expect_rejection(std::move(mutation), parsed.width, parsed.height, "oversized-frame-trailer");
    expect_rejection(packet.bytes, static_cast<std::uint16_t>(parsed.width + 2),
                     parsed.height, "caps-dimensions");
    return rejected;
}

std::size_t verify_entropy_rejections(const Packet& packet, const prores::Frame& parsed) {
    const auto& luma = parsed.slices.front().planes[0];
    if (luma.size < 4 || luma.offset > packet.bytes.size() - 4)
        throw std::runtime_error("first luma slice is too short for mutation");
    auto mutation = packet.bytes;
    const std::uint8_t oversized_dc[] = {0x00, 0x1f, 0xff, 0xf0};
    std::copy_n(oversized_dc, sizeof(oversized_dc), mutation.begin() + luma.offset);
    prores::Frame reparsed;
    std::string error;
    if (!prores::parse_frame(mutation.data(), mutation.size(), parsed.width,
                             parsed.height, reparsed, error))
        throw std::runtime_error("entropy mutation failed structural parse: " + error);
    std::vector<prores::CoefficientJob> jobs;
    std::vector<std::int32_t> coefficients;
    if (prores::make_coefficient_reference(mutation.data(), mutation.size(),
                                           reparsed, jobs, coefficients, error) ||
        error.find("first DC coefficient") == std::string::npos)
        throw std::runtime_error("oversized first DC was not rejected by CPU reference");
    return 1;
}

}  // namespace

int main(int argc, char** argv) try {
    if (argc < 2 || argc > 5)
        throw std::runtime_error("usage: prores_dx11_coeff input.mov [prores_vld.hlsl] [downloaded.raw] [--frame=N]");
    std::size_t frame_index = 0;
    const char* frame_argument = argc == 5 ? argv[4] :
        (argc == 4 && std::string(argv[3]).rfind("--frame=", 0) == 0 ? argv[3] : nullptr);
    if (frame_argument) {
        const std::string option(frame_argument);
        if (option.rfind("--frame=", 0) != 0) throw std::runtime_error("expected --frame=N");
        std::size_t consumed = 0;
        frame_index = std::stoull(option.substr(8), &consumed);
        if (consumed != option.size() - 8) throw std::runtime_error("invalid frame index");
    }
    const auto packet = read_video_packet(argv[1], frame_index);
    prores::Frame frame;
    std::string parse_error;
    if (!prores::parse_frame(packet.bytes.data(), packet.bytes.size(),
                             static_cast<std::uint16_t>(packet.width),
                             static_cast<std::uint16_t>(packet.height), frame, parse_error))
        throw std::runtime_error("parse: " + parse_error);
    std::vector<prores::CoefficientJob> jobs;
    std::vector<std::int32_t> reference;
    if (!prores::make_coefficient_reference(packet.bytes.data(), packet.bytes.size(),
                                            frame, jobs, reference, parse_error))
        throw std::runtime_error("CPU coefficients: " + parse_error);
    const auto malformed_rejections = verify_parser_rejections(packet, frame);
    const auto entropy_rejections = verify_entropy_rejections(packet, frame);
    std::vector<prores::IdctBlockJob> idct_jobs;
    prores::make_idct_jobs(frame, jobs, idct_jobs);
    if (idct_jobs.empty()) throw std::runtime_error("IDCT jobs missing");
    const auto cpu_frame = decode_cpu(packet);
    if (packet.bytes.size() > std::numeric_limits<UINT>::max() - 3 ||
        jobs.size() > std::numeric_limits<UINT>::max() / sizeof(prores::CoefficientJob) ||
        reference.size() > std::numeric_limits<UINT>::max() / sizeof(std::int32_t))
        throw std::runtime_error("D3D11 buffer size overflow");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested, 1,
                            D3D11_SDK_VERSION, &device, &feature_level, &context),
          "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    check(device.As(&dxgi_device), "Query IDXGIDevice");
    check(dxgi_device->GetAdapter(&adapter), "GetAdapter");
    check(adapter->GetDesc(&adapter_desc), "GetDesc");

    const wchar_t* shader_path = L"src/prores_vld.hlsl";
    std::wstring converted_path;
    if (argc >= 3) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, nullptr, 0);
        if (length <= 0) throw std::runtime_error("invalid UTF-8 shader path");
        converted_path.resize(static_cast<std::size_t>(length));
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, converted_path.data(), length);
        shader_path = converted_path.c_str();
    }
    auto bytecode = load_or_compile_shader(shader_path);
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                                      nullptr, &shader), "CreateComputeShader");

    auto padded_packet = packet.bytes;
    padded_packet.resize((padded_packet.size() + 3) & ~std::size_t(3), 0);
    auto frame_buffer = immutable_buffer(device.Get(), padded_packet, D3D11_BIND_SHADER_RESOURCE,
                                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
    auto job_buffer = immutable_buffer(device.Get(), jobs, D3D11_BIND_SHADER_RESOURCE,
                                       D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                       sizeof(prores::CoefficientJob));
    auto coefficient_buffer = output_buffer(device.Get(),
        static_cast<UINT>(reference.size() * sizeof(std::int32_t)), sizeof(std::int32_t));
    auto error_buffer = output_buffer(device.Get(),
        static_cast<UINT>(jobs.size() * sizeof(std::uint32_t)), sizeof(std::uint32_t));
    const std::vector<Parameters> parameter_values{{
        static_cast<std::uint32_t>(jobs.size()), static_cast<std::uint32_t>(reference.size())}};
    auto parameter_buffer = immutable_buffer(device.Get(), parameter_values,
                                              D3D11_BIND_CONSTANT_BUFFER);

    D3D11_SHADER_RESOURCE_VIEW_DESC raw_view_desc{};
    raw_view_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_view_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    raw_view_desc.BufferEx.NumElements = static_cast<UINT>(padded_packet.size() / 4);
    raw_view_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ComPtr<ID3D11ShaderResourceView> frame_view;
    check(device->CreateShaderResourceView(frame_buffer.Get(), &raw_view_desc, &frame_view),
          "Create frame SRV");
    ComPtr<ID3D11ShaderResourceView> job_view;
    check(device->CreateShaderResourceView(job_buffer.Get(), nullptr, &job_view),
          "Create job SRV");
    ComPtr<ID3D11UnorderedAccessView> coefficient_view;
    ComPtr<ID3D11UnorderedAccessView> error_view;
    check(device->CreateUnorderedAccessView(coefficient_buffer.Get(), nullptr, &coefficient_view),
          "Create coefficient UAV");
    check(device->CreateUnorderedAccessView(error_buffer.Get(), nullptr, &error_view),
          "Create error UAV");

    const UINT zeros[4]{};
    context->ClearUnorderedAccessViewUint(coefficient_view.Get(), zeros);
    context->ClearUnorderedAccessViewUint(error_view.Get(), zeros);
    ID3D11ShaderResourceView* srvs[] = {frame_view.Get(), job_view.Get()};
    ID3D11UnorderedAccessView* uavs[] = {coefficient_view.Get(), error_view.Get()};
    ID3D11Buffer* constant_buffers[] = {parameter_buffer.Get()};
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 2, srvs);
    context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    context->CSSetConstantBuffers(0, 1, constant_buffers);
    context->Dispatch(static_cast<UINT>((jobs.size() + 63) / 64), 1, 1);

    // The IDCT pass reads the coefficient UAV written above.  Unbinding it is
    // required before the same resource is rebound as an SRV; context command
    // ordering supplies the GPU dependency without a CPU wait or copy.
    ID3D11UnorderedAccessView* null_uavs[] = {nullptr, nullptr};
    ID3D11ShaderResourceView* null_srvs[] = {nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
    context->CSSetShaderResources(0, 2, null_srvs);

    // The production IDCT CSO targets R16_UNORM; this validator uses R16_UINT
    // to compare integer code values. Load the production VLD CSO but compile
    // the validator's R16_UINT IDCT source for the pixel cross-check.
    const auto idct_path = std::filesystem::path(shader_path).extension() == L".cso" ?
        std::filesystem::path(L"src/prores_idct.hlsl") :
        std::filesystem::path(shader_path).parent_path() / L"prores_idct.hlsl";
    auto idct_bytecode = load_or_compile_shader(idct_path.c_str());
    ComPtr<ID3D11ComputeShader> idct_shader;
    check(device->CreateComputeShader(idct_bytecode->GetBufferPointer(), idct_bytecode->GetBufferSize(),
                                      nullptr, &idct_shader), "Create IDCT shader");
    auto idct_job_buffer = immutable_buffer(device.Get(), idct_jobs, D3D11_BIND_SHADER_RESOURCE,
                                            D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                            sizeof(prores::IdctBlockJob));
    std::vector<std::uint32_t> quant_matrices;
    quant_matrices.reserve(128);
    for (auto value : frame.luma_quant_matrix) quant_matrices.push_back(value);
    for (auto value : frame.chroma_quant_matrix) quant_matrices.push_back(value);
    auto quant_buffer = immutable_buffer(device.Get(), quant_matrices, D3D11_BIND_SHADER_RESOURCE,
                                         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                                         sizeof(std::uint32_t));
    ComPtr<ID3D11ShaderResourceView> coefficient_srv;
    ComPtr<ID3D11ShaderResourceView> idct_job_srv;
    ComPtr<ID3D11ShaderResourceView> quant_srv;
    check(device->CreateShaderResourceView(coefficient_buffer.Get(), nullptr, &coefficient_srv),
          "Create coefficient SRV");
    check(device->CreateShaderResourceView(idct_job_buffer.Get(), nullptr, &idct_job_srv),
          "Create IDCT job SRV");
    check(device->CreateShaderResourceView(quant_buffer.Get(), nullptr, &quant_srv),
          "Create quant SRV");
    ComPtr<ID3D11Texture2D> output_textures[3];
    ComPtr<ID3D11UnorderedAccessView> output_uavs[3];
    for (unsigned component = 0; component < 3; ++component) {
        output_textures[component] = output_texture(device.Get(),
            component ? frame.width / 2 : frame.width, frame.height);
        check(device->CreateUnorderedAccessView(output_textures[component].Get(), nullptr,
                                                &output_uavs[component]),
              "Create output texture UAV");
        context->ClearUnorderedAccessViewUint(output_uavs[component].Get(), zeros);
    }
    const std::vector<IdctParameters> idct_parameter_values{{
        static_cast<std::uint32_t>(idct_jobs.size()), frame.width, frame.height}};
    auto idct_parameter_buffer = immutable_buffer(device.Get(), idct_parameter_values,
                                                   D3D11_BIND_CONSTANT_BUFFER);
    ID3D11ShaderResourceView* idct_srvs[] = {
        coefficient_srv.Get(), idct_job_srv.Get(), quant_srv.Get()};
    ID3D11UnorderedAccessView* idct_uavs[] = {
        output_uavs[0].Get(), output_uavs[1].Get(), output_uavs[2].Get()};
    ID3D11Buffer* idct_constants[] = {idct_parameter_buffer.Get()};
    context->CSSetShader(idct_shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 3, idct_srvs);
    context->CSSetUnorderedAccessViews(0, 3, idct_uavs, nullptr);
    context->CSSetConstantBuffers(0, 1, idct_constants);
    context->Dispatch(static_cast<UINT>(idct_jobs.size()), 1, 1);
    ID3D11UnorderedAccessView* null_idct_uavs[] = {nullptr, nullptr, nullptr};
    ID3D11ShaderResourceView* null_idct_srvs[] = {nullptr, nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 3, null_idct_uavs, nullptr);
    context->CSSetShaderResources(0, 3, null_idct_srvs);

    auto coefficient_staging = staging_buffer(device.Get(),
        static_cast<UINT>(reference.size() * sizeof(std::int32_t)));
    auto error_staging = staging_buffer(device.Get(),
        static_cast<UINT>(jobs.size() * sizeof(std::uint32_t)));
    context->CopyResource(coefficient_staging.Get(), coefficient_buffer.Get());
    context->CopyResource(error_staging.Get(), error_buffer.Get());
    ComPtr<ID3D11Texture2D> pixel_staging[3];
    for (unsigned component = 0; component < 3; ++component) {
        pixel_staging[component] = staging_texture(device.Get(),
            component ? frame.width / 2 : frame.width, frame.height);
        context->CopyResource(pixel_staging[component].Get(), output_textures[component].Get());
    }
    D3D11_MAPPED_SUBRESOURCE mapped_coefficients{};
    D3D11_MAPPED_SUBRESOURCE mapped_errors{};
    check(context->Map(coefficient_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped_coefficients),
          "Map coefficients");
    check(context->Map(error_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped_errors),
          "Map errors");
    const auto* gpu = static_cast<const std::int32_t*>(mapped_coefficients.pData);
    const auto* errors = static_cast<const std::uint32_t*>(mapped_errors.pData);
    std::size_t mismatch_count = 0;
    std::size_t first_mismatch = 0;
    std::int32_t maximum_difference = 0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const auto difference = static_cast<std::int32_t>(
            std::min<std::int64_t>(std::llabs(static_cast<long long>(gpu[i]) - reference[i]),
                                   std::numeric_limits<std::int32_t>::max()));
        maximum_difference = std::max(maximum_difference, difference);
        if (difference) {
            if (!mismatch_count) first_mismatch = i;
            ++mismatch_count;
        }
    }
    std::size_t shader_error_count = 0;
    for (std::size_t i = 0; i < jobs.size(); ++i) shader_error_count += errors[i] != 0;
    const auto first_gpu = mismatch_count ? gpu[first_mismatch] : 0;
    const auto first_cpu = mismatch_count ? reference[first_mismatch] : 0;
    context->Unmap(error_staging.Get(), 0);
    context->Unmap(coefficient_staging.Get(), 0);

    PixelDifference pixel_differences[3]{};
    for (unsigned component = 0; component < 3; ++component) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(pixel_staging[component].Get(), 0, D3D11_MAP_READ, 0, &mapped),
              "Map output texture");
        const auto plane_width = static_cast<std::uint32_t>(component ? frame.width / 2 : frame.width);
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            const auto* gpu_row = reinterpret_cast<const std::uint16_t*>(
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch);
            const auto* cpu_row = cpu_frame.planes[component].data() +
                                  static_cast<std::size_t>(y) * plane_width;
            for (std::uint32_t x = 0; x < plane_width; ++x) {
                if (!x && !y) {
                    pixel_differences[component].first_cpu = cpu_row[x];
                    pixel_differences[component].first_gpu = gpu_row[x];
                }
                pixel_differences[component].cpu_minimum =
                    std::min(pixel_differences[component].cpu_minimum, cpu_row[x]);
                pixel_differences[component].cpu_maximum =
                    std::max(pixel_differences[component].cpu_maximum, cpu_row[x]);
                pixel_differences[component].gpu_minimum =
                    std::min(pixel_differences[component].gpu_minimum, gpu_row[x]);
                pixel_differences[component].gpu_maximum =
                    std::max(pixel_differences[component].gpu_maximum, gpu_row[x]);
                const auto difference = static_cast<std::uint32_t>(
                    std::abs(static_cast<int>(gpu_row[x]) - static_cast<int>(cpu_row[x])));
                pixel_differences[component].maximum =
                    std::max(pixel_differences[component].maximum, difference);
                pixel_differences[component].absolute_sum += difference;
                pixel_differences[component].count += difference != 0;
            }
        }
        context->Unmap(pixel_staging[component].Get(), 0);
    }
    PixelDifference external_difference{};
    bool external_compared = false;
    if (argc == 5 || (argc == 4 && !frame_argument)) {
        std::ifstream raw(argv[3], std::ios::binary | std::ios::ate);
        if (!raw) throw std::runtime_error("cannot open external raw output");
        const auto expected_bytes = static_cast<std::streamoff>(
            (cpu_frame.planes[0].size() + cpu_frame.planes[1].size() +
             cpu_frame.planes[2].size()) * sizeof(std::uint16_t));
        if (raw.tellg() != expected_bytes) throw std::runtime_error("external raw size mismatch");
        raw.seekg(0);
        for (unsigned component = 0; component < 3; ++component) {
            std::vector<std::uint16_t> values(cpu_frame.planes[component].size());
            raw.read(reinterpret_cast<char*>(values.data()),
                     static_cast<std::streamsize>(values.size() * sizeof(std::uint16_t)));
            if (!raw) throw std::runtime_error("external raw read failed");
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto difference = static_cast<std::uint32_t>(std::abs(
                    static_cast<int>(values[i]) - static_cast<int>(cpu_frame.planes[component][i])));
                external_difference.maximum = std::max(external_difference.maximum, difference);
                external_difference.absolute_sum += difference;
                external_difference.count += difference != 0;
            }
        }
        external_compared = true;
    }

    char adapter_name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, adapter_name,
                        static_cast<int>(sizeof(adapter_name)), nullptr, nullptr);
    const bool coefficients_passed = !mismatch_count && !shader_error_count;
    const bool pixels_exact = !pixel_differences[0].count && !pixel_differences[1].count &&
                              !pixel_differences[2].count;
    const auto maximum_pixel_difference = std::max({pixel_differences[0].maximum,
        pixel_differences[1].maximum, pixel_differences[2].maximum});
    // The CPU decoder uses its integer/fixed-point IDCT while this shader uses
    // the directly evaluated floating-point transform.  One 10-bit code value
    // is therefore the explicit validation tolerance at the final rounding
    // boundary; coefficients themselves remain bit exact.
    constexpr std::uint32_t pixel_tolerance = 1;
    const bool passed = coefficients_passed && maximum_pixel_difference <= pixel_tolerance &&
                        malformed_rejections == 12 && entropy_rejections == 1 &&
                        (!external_compared || external_difference.maximum <= pixel_tolerance);
    std::cout << "{\"passed\":" << (passed ? "true" : "false")
              << ",\"frame_index\":" << frame_index
              << ",\"adapter\":\"" << adapter_name << "\""
              << ",\"feature_level\":" << feature_level
              << ",\"width\":" << frame.width << ",\"height\":" << frame.height
              << ",\"slices\":" << frame.slices.size()
              << ",\"jobs\":" << jobs.size()
              << ",\"coefficients\":" << reference.size()
              << ",\"mismatches\":" << mismatch_count
              << ",\"max_abs_difference\":" << maximum_difference
              << ",\"shader_errors\":" << shader_error_count;
    if (mismatch_count)
        std::cout << ",\"first_mismatch\":" << first_mismatch
                  << ",\"cpu\":" << first_cpu << ",\"gpu\":" << first_gpu;
    std::cout << ",\"d3d11_texture_format\":\"R16_UINT\",\"gpu_internal_copies\":0"
              << ",\"normal_path_cpu_readback\":false,\"validation_readback\":true"
              << ",\"pixel_reference_exact\":" << (pixels_exact ? "true" : "false")
              << ",\"pixel_tolerance\":" << pixel_tolerance
              << ",\"tolerance_basis\":\"floating-point separable IDCT versus FFmpeg integer IDCT final rounding\""
              << ",\"malformed_cases_rejected\":" << malformed_rejections
              << ",\"entropy_cases_rejected\":" << entropy_rejections
              << ",\"pixel_differences\":[";
    for (unsigned component = 0; component < 3; ++component) {
        if (component) std::cout << ',';
        const auto samples = cpu_frame.planes[component].size();
        std::cout << "{\"plane\":\"" << "YUV"[component] << "\",\"different\":"
                  << pixel_differences[component].count << ",\"samples\":" << samples
                  << ",\"max_abs\":" << pixel_differences[component].maximum
                  << ",\"cpu_min\":" << pixel_differences[component].cpu_minimum
                  << ",\"cpu_max\":" << pixel_differences[component].cpu_maximum
                  << ",\"gpu_min\":" << pixel_differences[component].gpu_minimum
                  << ",\"gpu_max\":" << pixel_differences[component].gpu_maximum
                  << ",\"first_cpu\":" << pixel_differences[component].first_cpu
                  << ",\"first_gpu\":" << pixel_differences[component].first_gpu
                  << ",\"mae\":" << (samples ? static_cast<double>(pixel_differences[component].absolute_sum) / samples : 0.0)
                  << '}';
    }
    std::cout << ']';
    if (external_compared) {
        const auto external_samples = cpu_frame.planes[0].size() + cpu_frame.planes[1].size() +
                                      cpu_frame.planes[2].size();
        std::cout << ",\"external_raw\":{\"different\":" << external_difference.count
                  << ",\"samples\":" << external_samples
                  << ",\"max_abs\":" << external_difference.maximum
                  << ",\"mae\":" << static_cast<double>(external_difference.absolute_sum) / external_samples
                  << '}';
    }
    std::cout << "}\n";
    return passed ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
