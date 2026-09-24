// 実ProRes packetのalpha全画素をFFmpeg固定SDKのCPU復号とDX11 CSで照合する。
#include "prores_parser.hpp"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
void check(HRESULT hr, const char* label) {
    if (FAILED(hr)) throw std::runtime_error(std::string(label) + " HRESULT=" +
                                              std::to_string(static_cast<unsigned>(hr)));
}
void require(bool value, const char* label) {
    if (!value) throw std::runtime_error(label);
}
struct AlphaJob {
    std::uint32_t data_offset, data_size, mb_x, mb_y, mb_count, field_layout;
};
struct Parameters {
    std::uint32_t job_count, width, height, alpha_info_and_depth;
    std::uint32_t error_base, reserved[3]{};
};
struct Packet {
    std::vector<std::uint8_t> bytes;
    int width = 0, height = 0;
    unsigned codec_tag = 0;
};

Packet read_packet(const char* filename, unsigned frame_number) {
    AVFormatContext* raw = nullptr;
    require(avformat_open_input(&raw, filename, nullptr, nullptr) >= 0, "cannot open ProRes file");
    auto close = [](AVFormatContext* value) { avformat_close_input(&value); };
    std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
    require(avformat_find_stream_info(format.get(), nullptr) >= 0, "stream info failed");
    const int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    require(stream >= 0, "video stream missing");
    const auto* codec = format->streams[stream]->codecpar;
    require(codec->codec_id == AV_CODEC_ID_PRORES, "input is not ProRes");
    AVPacket* raw_packet = av_packet_alloc();
    require(raw_packet != nullptr, "packet allocation failed");
    auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(raw_packet, free_packet);
    unsigned index = 0;
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == stream && index++ == frame_number)
            return {{packet->data, packet->data + packet->size}, codec->width,
                    codec->height, codec->codec_tag};
        av_packet_unref(packet.get());
    }
    throw std::runtime_error("requested frame is missing");
}

std::vector<std::uint16_t> decode_cpu_alpha(const Packet& packet) {
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_PRORES);
    require(codec != nullptr, "CPU reference decoder missing");
    AVCodecContext* raw_context = avcodec_alloc_context3(codec);
    require(raw_context != nullptr, "decoder context allocation failed");
    auto free_context = [](AVCodecContext* value) { avcodec_free_context(&value); };
    std::unique_ptr<AVCodecContext, decltype(free_context)> context(raw_context, free_context);
    context->width = packet.width;
    context->height = packet.height;
    context->codec_tag = packet.codec_tag;
    context->thread_count = 1;
    require(avcodec_open2(context.get(), codec, nullptr) >= 0, "CPU decoder open failed");
    AVPacket* raw_packet = av_packet_alloc();
    AVFrame* raw_frame = av_frame_alloc();
    require(raw_packet && raw_frame, "CPU reference allocation failed");
    auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    auto free_frame = [](AVFrame* value) { av_frame_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> input(raw_packet, free_packet);
    std::unique_ptr<AVFrame, decltype(free_frame)> frame(raw_frame, free_frame);
    require(av_new_packet(input.get(), static_cast<int>(packet.bytes.size())) >= 0,
            "CPU packet allocation failed");
    std::memcpy(input->data, packet.bytes.data(), packet.bytes.size());
    require(avcodec_send_packet(context.get(), input.get()) >= 0 &&
            avcodec_receive_frame(context.get(), frame.get()) >= 0, "CPU decode failed");
    require(frame->data[3] != nullptr && frame->width == packet.width &&
            frame->height == packet.height, "CPU alpha plane missing");
    std::vector<std::uint16_t> output(static_cast<std::size_t>(packet.width) * packet.height);
    for (int y = 0; y < packet.height; ++y)
        std::memcpy(output.data() + static_cast<std::size_t>(y) * packet.width,
                    frame->data[3] + static_cast<std::size_t>(y) * frame->linesize[3],
                    static_cast<std::size_t>(packet.width) * sizeof(std::uint16_t));
    return output;
}

ComPtr<ID3D11Buffer> buffer(ID3D11Device* device, UINT bytes, UINT bind,
                            UINT misc, UINT stride, const void* initial = nullptr) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = initial ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = misc;
    desc.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA data{initial, 0, 0};
    ComPtr<ID3D11Buffer> result;
    check(device->CreateBuffer(&desc, initial ? &data : nullptr, &result), "CreateBuffer");
    return result;
}
}  // namespace

int main(int argc, char** argv) try {
    require(argc >= 3 && argc <= 4, "usage: prores_dx11_alpha input.mov prores_alpha.cso [frame-index]");
    const unsigned frame_number = argc == 4 ? static_cast<unsigned>(std::stoul(argv[3])) : 0;
    const Packet packet = read_packet(argv[1], frame_number);
    const auto bit_depth = packet.codec_tag == MKTAG('a','p','4','h') ||
        packet.codec_tag == MKTAG('a','p','4','x') ? 12 : 10;
    prores::Frame parsed;
    std::string error;
    require(prores::parse_frame(packet.bytes.data(), packet.bytes.size(),
        static_cast<std::uint16_t>(packet.width), static_cast<std::uint16_t>(packet.height),
        parsed, error, static_cast<std::uint8_t>(bit_depth), true), error.c_str());
    require(parsed.alpha_info == 1 || parsed.alpha_info == 2, "input has no alpha");
    const auto reference = decode_cpu_alpha(packet);
    std::vector<AlphaJob> jobs;
    for (const auto& slice : parsed.slices) {
        const auto& alpha = slice.planes[3];
        jobs.push_back({alpha.offset, alpha.size, slice.mb_x, slice.mb_y, slice.mb_count,
                        (parsed.frame_type ? 2u : 1u) |
                            (static_cast<std::uint32_t>(slice.field_parity) << 8)});
    }

    std::ifstream shader_file(argv[2], std::ios::binary | std::ios::ate);
    require(shader_file.good(), "alpha shader file missing");
    std::vector<char> shader(static_cast<std::size_t>(shader_file.tellg()));
    shader_file.seekg(0);
    shader_file.read(shader.data(), shader.size());
    require(shader_file.good(), "alpha shader read failed");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        requested, 1, D3D11_SDK_VERSION, &device, &level, &context), "D3D11CreateDevice");
    ComPtr<ID3D11ComputeShader> program;
    check(device->CreateComputeShader(shader.data(), shader.size(), nullptr, &program),
          "CreateComputeShader alpha");

    auto padded = packet.bytes;
    padded.resize((padded.size() + 3) & ~std::size_t(3));
    const auto packet_buffer = buffer(device.Get(), static_cast<UINT>(padded.size()),
        D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, padded.data());
    const auto job_buffer = buffer(device.Get(), static_cast<UINT>(jobs.size() * sizeof(AlphaJob)),
        D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(AlphaJob), jobs.data());
    const Parameters parameter_values{static_cast<UINT>(jobs.size()), parsed.width, parsed.height,
        static_cast<UINT>((bit_depth << 8) | parsed.alpha_info), 0};
    const auto parameters = buffer(device.Get(), sizeof(Parameters), D3D11_BIND_CONSTANT_BUFFER,
        0, 0, &parameter_values);
    const auto errors = buffer(device.Get(), static_cast<UINT>(jobs.size() * sizeof(UINT)),
        D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(UINT));
    ComPtr<ID3D11ShaderResourceView> packet_srv, job_srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC raw_view{};
    raw_view.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    raw_view.BufferEx.NumElements = static_cast<UINT>(padded.size() / 4);
    raw_view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    check(device->CreateShaderResourceView(packet_buffer.Get(), &raw_view, &packet_srv),
          "CreateShaderResourceView packet");
    check(device->CreateShaderResourceView(job_buffer.Get(), nullptr, &job_srv),
          "CreateShaderResourceView jobs");
    ComPtr<ID3D11UnorderedAccessView> error_uav, alpha_uav;
    check(device->CreateUnorderedAccessView(errors.Get(), nullptr, &error_uav),
          "CreateUnorderedAccessView errors");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = parsed.width;
    desc.Height = parsed.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> alpha;
    check(device->CreateTexture2D(&desc, nullptr, &alpha), "CreateTexture2D alpha");
    check(device->CreateUnorderedAccessView(alpha.Get(), nullptr, &alpha_uav),
          "CreateUnorderedAccessView alpha");
    const UINT zeros[4]{};
    context->ClearUnorderedAccessViewUint(error_uav.Get(), zeros);
    ID3D11ShaderResourceView* srvs[] = {packet_srv.Get(), job_srv.Get()};
    ID3D11UnorderedAccessView* uavs[] = {alpha_uav.Get(), error_uav.Get()};
    ID3D11Buffer* constants[] = {parameters.Get()};
    context->CSSetShader(program.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 2, srvs);
    context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    context->CSSetConstantBuffers(0, 1, constants);
    context->Dispatch(static_cast<UINT>((jobs.size() + 63) / 64), 1, 1);
    ID3D11UnorderedAccessView* null_uavs[] = {nullptr, nullptr};
    context->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D staging");
    context->CopyResource(staging.Get(), alpha.Get());
    D3D11_BUFFER_DESC error_desc{};
    errors->GetDesc(&error_desc);
    error_desc.Usage = D3D11_USAGE_STAGING;
    error_desc.BindFlags = 0;
    error_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    error_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ComPtr<ID3D11Buffer> error_staging;
    check(device->CreateBuffer(&error_desc, nullptr, &error_staging), "CreateBuffer error staging");
    context->CopyResource(error_staging.Get(), errors.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(error_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map errors");
    std::size_t rejected = 0;
    for (std::size_t i = 0; i < jobs.size(); ++i)
        rejected += static_cast<const UINT*>(mapped.pData)[i] != 0;
    context->Unmap(error_staging.Get(), 0);
    require(!rejected, "GPU alpha entropy rejected one or more slices");
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map alpha");
    std::size_t mismatches = 0;
    unsigned max_difference = 0;
    for (int y = 0; y < packet.height; ++y) {
        const auto* row = reinterpret_cast<const std::uint16_t*>(
            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (int x = 0; x < packet.width; ++x) {
            const auto decoded = reference[static_cast<std::size_t>(y) * packet.width + x];
            const auto actual = row[x];
            // FFmpeg drops the low 6/4 bits of 16-bit alpha in 10/12-bit output.
            // Its 8-bit path replicates into the output depth; recover the 8-bit source.
            const auto expected = parsed.alpha_info == 2 ?
                static_cast<std::uint16_t>(decoded << (16 - bit_depth)) :
                static_cast<std::uint16_t>(((decoded >> (bit_depth - 8)) << 8) |
                                           (decoded >> (bit_depth - 8)));
            const auto comparable = parsed.alpha_info == 2 ?
                static_cast<std::uint16_t>(actual >> (16 - bit_depth)) : actual;
            const auto reference_value = parsed.alpha_info == 2 ? decoded : expected;
            const auto difference = comparable > reference_value ?
                comparable - reference_value : reference_value - comparable;
            max_difference = std::max(max_difference, static_cast<unsigned>(difference));
            mismatches += difference != 0;
            if (parsed.alpha_info == 1) mismatches += actual != expected;
        }
    }
    context->Unmap(staging.Get(), 0);
    std::cout << "{\"file\":\"" << std::filesystem::path(argv[1]).filename().string()
              << "\",\"frame\":" << frame_number
              << ",\"alpha_bits\":" << (parsed.alpha_info == 2 ? 16 : 8)
              << ",\"output_bits\":" << bit_depth << ",\"slices\":" << jobs.size()
              << ",\"pixels\":" << reference.size() << ",\"mismatches\":" << mismatches
              << ",\"max_difference\":" << max_difference << "}\n";
    require(!mismatches, "GPU alpha pixels differ from CPU reference");
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
