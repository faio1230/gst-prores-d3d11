// 422 YUV packetと同寸法・同スライス配置の444 alpha packetを合成し、
// FFmpeg/VulkanとDX11の交差条件検証用MOVを作る。画像復号は行わない。
#include "prores_parser.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
void put16(std::uint8_t* p, unsigned value) {
    require(value <= 65535, "ProRes slice exceeds 16-bit size");
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
}
void put32(std::uint8_t* p, unsigned value) {
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
}
struct Input {
    AVFormatContext* format = nullptr;
    int stream = -1;
    explicit Input(const char* path) {
        require(avformat_open_input(&format, path, nullptr, nullptr) >= 0,
                "cannot open input MOV");
        require(avformat_find_stream_info(format, nullptr) >= 0,
                "cannot read input stream info");
        stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        require(stream >= 0 && format->streams[stream]->codecpar->codec_id == AV_CODEC_ID_PRORES,
                "input has no ProRes video stream");
    }
    ~Input() { avformat_close_input(&format); }
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
};
bool next_video(Input& input, AVPacket* packet) {
    av_packet_unref(packet);
    while (av_read_frame(input.format, packet) >= 0) {
        if (packet->stream_index == input.stream) return true;
        av_packet_unref(packet);
    }
    return false;
}
std::vector<std::uint8_t> combine(const AVPacket* base, const AVPacket* alpha,
                                  int width, int height, std::uint8_t depth) {
    require(width > 0 && width <= 65535 && height > 0 && height <= 65535,
            "input dimensions exceed parser range");
    const auto parsed_width = static_cast<std::uint16_t>(width);
    const auto parsed_height = static_cast<std::uint16_t>(height);
    prores::Frame yuv_frame, alpha_frame;
    std::string error;
    require(prores::parse_frame(base->data, base->size, parsed_width, parsed_height,
                               yuv_frame, error, depth), error.c_str());
    require(prores::parse_frame(alpha->data, alpha->size, parsed_width, parsed_height,
                               alpha_frame, error, depth, true), error.c_str());
    require(yuv_frame.chroma_shift == 1 && alpha_frame.alpha_info != 0 &&
            yuv_frame.slices.size() == alpha_frame.slices.size(),
            "inputs must be matching 422 YUV and alpha frames");
    const auto picture = 8u + be16(base->data + 8);
    const auto picture_header = base->data[picture] >> 3;
    const auto index_end = picture + picture_header + 2 * yuv_frame.slices.size();
    require(index_end <= static_cast<unsigned>(base->size), "invalid base slice table");
    std::vector<std::uint8_t> output(base->data, base->data + index_end);
    output[8 + 17] = static_cast<std::uint8_t>((output[8 + 17] & 0xf0) |
                                               alpha_frame.alpha_info);
    for (std::size_t i = 0; i < yuv_frame.slices.size(); ++i) {
        const auto& yuv = yuv_frame.slices[i];
        const auto& a = alpha_frame.slices[i];
        require(yuv.mb_x == a.mb_x && yuv.mb_y == a.mb_y &&
                yuv.mb_count == a.mb_count, "slice layouts differ");
        const unsigned size = 8 + yuv.planes[0].size + yuv.planes[1].size +
            yuv.planes[2].size + a.planes[3].size;
        put16(output.data() + picture + picture_header + 2 * i, size);
        const auto* header = base->data + yuv.offset;
        const auto old_header_size = header[0] >> 3;
        require(old_header_size >= 6, "base slice header is invalid");
        output.push_back(static_cast<std::uint8_t>((header[0] & 7) | (8 << 3)));
        output.insert(output.end(), header + 1, header + 6);
        output.resize(output.size() + 2);
        put16(output.data() + output.size() - 2, yuv.planes[2].size);
        for (unsigned component = 0; component < 4; ++component) {
            const auto& plane = component == 3 ? a.planes[3] : yuv.planes[component];
            const auto* bytes = component == 3 ? alpha->data : base->data;
            output.insert(output.end(), bytes + plane.offset, bytes + plane.offset + plane.size);
        }
    }
    put32(output.data(), static_cast<unsigned>(output.size()));
    put32(output.data() + picture + 1, static_cast<unsigned>(output.size() - picture));
    prores::Frame verified;
    require(prores::parse_frame(output.data(), output.size(), parsed_width, parsed_height,
                               verified, error, depth, true), error.c_str());
    require(verified.chroma_shift == 1 && verified.alpha_info == alpha_frame.alpha_info,
            "combined frame failed structural verification");
    return output;
}
}  // namespace

int main(int argc, char** argv) try {
    require(argc == 4, "usage: make_prores_422_alpha base422.mov alpha444.mov output.mov");
    Input base(argv[1]), alpha(argv[2]);
    const auto* codec = base.format->streams[base.stream]->codecpar;
    const auto* alpha_codec = alpha.format->streams[alpha.stream]->codecpar;
    require(codec->width == alpha_codec->width && codec->height == alpha_codec->height &&
            codec->codec_tag == alpha_codec->codec_tag,
            "input dimensions/FourCC must match");
    const auto depth = codec->codec_tag == MKTAG('a','p','4','h') ||
        codec->codec_tag == MKTAG('a','p','4','x') ? 12u : 10u;
    AVFormatContext* raw_output = nullptr;
    require(avformat_alloc_output_context2(&raw_output, nullptr, nullptr, argv[3]) >= 0 &&
            raw_output != nullptr, "cannot create output MOV");
    auto free_output = [](AVFormatContext* value) {
        if (value->pb) avio_closep(&value->pb);
        avformat_free_context(value);
    };
    std::unique_ptr<AVFormatContext, decltype(free_output)> output(raw_output, free_output);
    AVStream* stream = avformat_new_stream(output.get(), nullptr);
    require(stream != nullptr &&
            avcodec_parameters_copy(stream->codecpar, codec) >= 0,
            "cannot copy ProRes stream parameters");
    stream->time_base = base.format->streams[base.stream]->time_base;
    require(avio_open(&output->pb, argv[3], AVIO_FLAG_WRITE) >= 0,
            "cannot open output MOV for writing");
    require(avformat_write_header(output.get(), nullptr) >= 0, "cannot write MOV header");
    AVPacket* raw_base = av_packet_alloc();
    AVPacket* raw_alpha = av_packet_alloc();
    require(raw_base && raw_alpha, "packet allocation failed");
    auto free_packet = [](AVPacket* value) { av_packet_free(&value); };
    std::unique_ptr<AVPacket, decltype(free_packet)> base_packet(raw_base, free_packet);
    std::unique_ptr<AVPacket, decltype(free_packet)> alpha_packet(raw_alpha, free_packet);
    unsigned frames = 0;
    while (next_video(base, base_packet.get())) {
        require(next_video(alpha, alpha_packet.get()), "alpha input has fewer frames");
        auto bytes = combine(base_packet.get(), alpha_packet.get(), codec->width,
                             codec->height, static_cast<std::uint8_t>(depth));
        AVPacket* raw_combined = av_packet_alloc();
        require(raw_combined != nullptr, "combined packet allocation failed");
        std::unique_ptr<AVPacket, decltype(free_packet)> combined(raw_combined, free_packet);
        require(av_new_packet(combined.get(), static_cast<int>(bytes.size())) >= 0,
                "combined packet data allocation failed");
        std::memcpy(combined->data, bytes.data(), bytes.size());
        combined->stream_index = stream->index;
        combined->pts = base_packet->pts;
        combined->dts = base_packet->dts;
        combined->duration = base_packet->duration;
        combined->flags = base_packet->flags;
        av_packet_rescale_ts(combined.get(), base.format->streams[base.stream]->time_base,
                             stream->time_base);
        require(av_interleaved_write_frame(output.get(), combined.get()) >= 0,
                "cannot mux combined ProRes packet");
        ++frames;
    }
    require(!next_video(alpha, alpha_packet.get()), "alpha input has more frames");
    require(frames != 0 && av_write_trailer(output.get()) >= 0,
            "cannot finalize combined MOV");
    std::cout << "{\"frames\":" << frames << ",\"width\":" << codec->width
              << ",\"height\":" << codec->height << ",\"depth\":" << depth << "}\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
