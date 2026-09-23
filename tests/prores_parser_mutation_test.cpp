// 固定ProRes素材の1フレームへ決定的な変異を加え、CPU境界検査を再現する。
#include "prores_parser.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t be16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

void put_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

struct Random {
    std::uint32_t state = 0x7da31f59u;
    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    std::size_t index(std::size_t size) { return static_cast<std::size_t>(next()) % size; }
};

std::vector<std::uint8_t> read_frame(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.good(), "cannot open frame: " + path);
    const auto length = input.tellg();
    require(length > 0 && length <= 128 * 1024 * 1024, "invalid seed size: " + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), length);
    require(input.good(), "cannot read frame: " + path);
    return bytes;
}

void check_frame_bounds(const prores::Frame& frame, std::size_t size) {
    std::uint64_t covered = 0;
    for (const auto& slice : frame.slices) {
        require(slice.offset <= size && slice.size <= size - slice.offset,
                "slice exceeds frame");
        const auto slice_end = static_cast<std::size_t>(slice.offset) + slice.size;
        require(slice.mb_count >= 1 && slice.mb_count <= 8, "invalid macroblock count");
        require(static_cast<unsigned>(slice.mb_x) + slice.mb_count <= frame.mb_width &&
                slice.mb_y < frame.mb_height, "slice exceeds picture grid");
        covered += slice.mb_count;
        for (const auto& plane : slice.planes)
            require(plane.offset >= slice.offset && plane.offset <= slice_end &&
                    plane.offset <= size &&
                    plane.size <= size - plane.offset &&
                    plane.size <= slice_end - plane.offset,
                    "plane exceeds slice");
    }
    require(covered == static_cast<std::uint64_t>(frame.mb_width) * frame.mb_height,
            "slice coverage differs from picture grid");
    std::vector<prores::CoefficientJob> jobs;
    std::uint32_t coefficient_count = 0;
    prores::make_coefficient_jobs(frame, jobs, coefficient_count);
    require(jobs.size() == frame.slices.size() * 3 &&
            coefficient_count == covered * 512, "coefficient job count differs");
    for (const auto& job : jobs)
        require(job.data_offset <= size && job.data_size <= size - job.data_offset &&
                job.output_offset <= coefficient_count &&
                job.block_count * 64 <= coefficient_count - job.output_offset,
                "coefficient job exceeds frame or output");
    std::vector<prores::IdctBlockJob> idct_jobs;
    prores::make_idct_jobs(frame, jobs, idct_jobs);
    require(idct_jobs.size() == coefficient_count / 64, "IDCT job count differs");
}

void mutate(std::vector<std::uint8_t>& bytes, const prores::Frame& seed,
            Random& random, unsigned mode) {
    const auto position = random.index(bytes.size());
    switch (mode) {
    case 0: bytes[position] ^= static_cast<std::uint8_t>(1u << random.index(8)); break;
    case 1: bytes[position] = 0; break;
    case 2: bytes[position] = 0xff; break;
    case 3: {
        const std::size_t offsets[] = {0, 3, 8, 9, 10, 11, 16, 19, 20, 21, 22, 24, 25, 26};
        bytes[offsets[random.index(std::size(offsets))]] =
            static_cast<std::uint8_t>(random.next());
        break;
    }
    case 4: {
        const auto length = std::min<std::size_t>(bytes.size() - position,
                                                  1 + random.index(32));
        std::fill_n(bytes.begin() + position, length, static_cast<std::uint8_t>(0));
        break;
    }
    case 5: {
        const auto length = random.index(bytes.size());
        bytes.resize(length);
        if (length >= 4) put_be32(bytes, static_cast<std::uint32_t>(length));
        break;
    }
    case 6: {
        const auto picture = 8 + be16(bytes.data() + 8);
        const auto index = picture + (bytes[picture] >> 3);
        const auto chosen = random.index(seed.slices.size());
        const auto offset = random.next() & 1 ? index + chosen * 2 :
                            static_cast<std::size_t>(seed.slices[chosen].offset);
        bytes[offset + random.index(2)] = static_cast<std::uint8_t>(random.next());
        break;
    }
    case 7: {
        const auto& slice = seed.slices[random.index(seed.slices.size())];
        const auto& plane = slice.planes[random.index(3)];
        if (plane.size) {
            const auto offset = plane.offset + random.index(plane.size);
            bytes[offset] ^= static_cast<std::uint8_t>(1u << random.index(8));
        }
        break;
    }
    default:
        for (unsigned i = 0; i < 4; ++i)
            bytes[random.index(bytes.size())] = static_cast<std::uint8_t>(random.next());
        break;
    }
}

}  // namespace

int main(int argc, char** argv) try {
    require(argc >= 4, "usage: prores_parser_mutation_test ITERATIONS BOUNDARY_PACKET FRAME...");
    const auto iterations = std::stoul(argv[1]);
    require(iterations >= 1 && iterations <= 100000, "iterations outside 1..100000");
    Random random;
    std::uint64_t structural_rejected = 0;
    std::uint64_t structural_accepted = 0;
    std::uint64_t entropy_checked = 0;
    std::uint64_t entropy_rejected = 0;
    std::uint64_t boundary_rejected = 0;
    bool boundary_written = false;
    unsigned long boundary_case = 0;
    for (int source = 3; source < argc; ++source) {
        const auto original = read_frame(argv[source]);
        prores::Frame seed;
        std::string error;
        require(prores::parse_frame(original.data(), original.size(), 0, 0, seed, error),
                std::string("invalid seed: ") + argv[source] + ": " + error);
        check_frame_bounds(seed, original.size());
        for (unsigned long i = 0; i < iterations; ++i) {
            const auto mode = static_cast<unsigned>(i % 9);
            auto bytes = original;
            mutate(bytes, seed, random, mode);
            prores::Frame frame;
            const bool accepted = prores::parse_frame(bytes.data(), bytes.size(),
                seed.width, seed.height, frame, error);
            if (!accepted) { ++structural_rejected; continue; }
            ++structural_accepted;
            try {
                check_frame_bounds(frame, bytes.size());
                if (mode == 7 || (i & 15) == 0) {
                    std::vector<prores::CoefficientJob> jobs;
                    std::vector<std::int32_t> coefficients;
                    ++entropy_checked;
                    if (!prores::make_coefficient_reference(bytes.data(), bytes.size(), frame,
                                                            jobs, coefficients, error)) {
                        ++entropy_rejected;
                        if (error.find("AC run reaches coefficient plane end") != std::string::npos) {
                            ++boundary_rejected;
                            if (source == 3 && !boundary_written) {
                                std::ofstream packet(argv[2], std::ios::binary);
                                require(packet.good(), "cannot write AC boundary packet");
                                packet.write(reinterpret_cast<const char*>(bytes.data()),
                                             static_cast<std::streamsize>(bytes.size()));
                                require(packet.good(), "cannot write AC boundary packet data");
                                boundary_written = true;
                                boundary_case = i;
                            }
                        }
                    }
                }
            } catch (const std::exception& failure) {
                throw std::runtime_error(std::string(argv[source]) + " case=" +
                    std::to_string(i) + " mode=" + std::to_string(mode) + ": " + failure.what());
            }
        }
    }
    require(boundary_written, "no AC boundary packet found in the first seed");
    std::cout << "{\"passed\":true,\"asan_enabled\":"
#ifdef PRORES_PARSER_ASAN
              << "true"
#else
              << "false"
#endif
              << ",\"seed_count\":" << argc - 3
              << ",\"cases\":" << iterations * static_cast<unsigned long>(argc - 3)
              << ",\"structural_rejected\":" << structural_rejected
              << ",\"structural_accepted\":" << structural_accepted
              << ",\"entropy_checked\":" << entropy_checked
              << ",\"entropy_rejected\":" << entropy_rejected
              << ",\"boundary_rejected\":" << boundary_rejected
              << ",\"boundary_case_first_seed\":" << boundary_case << "}\n";
    return 0;
} catch (const std::exception& failure) {
    std::cerr << "FAIL: " << failure.what() << '\n';
    return 1;
}
