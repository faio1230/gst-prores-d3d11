/*
 * ProRes entropy parsing based on FFmpeg libavcodec/proresdec.c.
 * Copyright (c) 2010-2011 Maxim Poliakovski
 * Copyright (c) 2010-2011 Elvis Presley
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "prores_parser.hpp"

#include <algorithm>
#include <limits>
#include <sstream>

namespace prores {
namespace {

constexpr std::array<std::uint8_t, 64> kProgressiveScan = {
     0,  1,  8,  9,  2,  3, 10, 11,
    16, 17, 24, 25, 18, 19, 26, 27,
     4,  5, 12, 20, 13,  6,  7, 14,
    21, 28, 29, 22, 15, 23, 30, 31,
    32, 33, 40, 48, 41, 34, 35, 42,
    49, 56, 57, 50, 43, 36, 37, 44,
    51, 58, 59, 52, 45, 38, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

constexpr std::array<std::uint8_t, 7> kDcCodebook = {
    0x04, 0x28, 0x28, 0x4d, 0x4d, 0x70, 0x70,
};
constexpr std::array<std::uint8_t, 16> kRunCodebook = {
    0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29,
    0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4c,
};
constexpr std::array<std::uint8_t, 10> kLevelCodebook = {
    0x04, 0x0a, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4c,
};

std::uint16_t read_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

unsigned population_count(unsigned value) {
    unsigned count = 0;
    while (value) {
        value &= value - 1;
        ++count;
    }
    return count;
}

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t bytes)
        : data_(data), bit_size_(bytes * 8) {}

    std::size_t bits_left() const { return bit_size_ - bit_position_; }

    std::uint32_t show(unsigned count) const {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value <<= 1;
            const auto position = bit_position_ + i;
            if (position < bit_size_)
                value |= (data_[position >> 3] >> (7 - (position & 7))) & 1;
        }
        return value;
    }

    bool get(unsigned count, std::uint32_t& value) {
        if (count > 32 || count > bits_left()) return false;
        value = show(count);
        bit_position_ += count;
        return true;
    }

    bool skip(unsigned count) {
        if (count > bits_left()) return false;
        bit_position_ += count;
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t bit_size_;
    std::size_t bit_position_ = 0;
};

bool decode_codeword(BitReader& bits, std::uint8_t codebook, std::uint32_t& value) {
    const unsigned switch_bits = codebook & 3;
    const unsigned rice_order = codebook >> 5;
    const unsigned exp_order = (codebook >> 2) & 7;
    unsigned q = 0;
    while (q < 32 && ((bits.show(q + 1) & 1) == 0)) ++q;
    if (q == 32) return false;

    if (q > switch_bits) {
        const unsigned count = exp_order - switch_bits + q * 2;
        if (count > 31) return false;
        std::uint32_t encoded = 0;
        if (!bits.get(count, encoded)) return false;
        value = encoded - (1u << exp_order) + ((switch_bits + 1) << rice_order);
    } else if (rice_order) {
        std::uint32_t remainder = 0;
        if (!bits.skip(q + 1) || !bits.get(rice_order, remainder)) return false;
        value = (q << rice_order) + remainder;
    } else {
        value = q;
        if (!bits.skip(q + 1)) return false;
    }
    return true;
}

std::int32_t to_signed(std::uint32_t value) {
    return static_cast<std::int32_t>((value >> 1) ^
        static_cast<std::uint32_t>(-static_cast<std::int32_t>(value & 1)));
}

bool decode_plane(const std::uint8_t* data, std::size_t size,
                  std::uint32_t block_count, std::int32_t* output,
                  std::string& error) {
    if (!block_count || block_count > 32 || (block_count & (block_count - 1)))
        return fail(error, "invalid coefficient block count");
    if (!size) {
        // Empty chroma scans are a de-facto grayscale extension.  Luma may not
        // use it, which is checked by the frame parser.
        return true;
    }

    BitReader bits(data, size);
    std::uint32_t code = 0;
    if (!decode_codeword(bits, 0xb8, code))
        return fail(error, "truncated first DC coefficient");
    std::int32_t previous_dc = to_signed(code);
    output[0] = previous_dc;

    code = 5;
    std::int32_t sign = 0;
    for (std::uint32_t block = 1; block < block_count; ++block) {
        if (!decode_codeword(bits, kDcCodebook[std::min<std::uint32_t>(code, 6)], code))
            return fail(error, "truncated DC coefficient");
        if (code) sign ^= -static_cast<std::int32_t>(code & 1);
        else sign = 0;
        previous_dc += (static_cast<std::int32_t>((code + 1) >> 1) ^ sign) - sign;
        if (previous_dc < std::numeric_limits<std::int16_t>::min() ||
            previous_dc > std::numeric_limits<std::int16_t>::max())
            return fail(error, "DC coefficient exceeds signed 16-bit range");
        output[block * 64] = previous_dc;
    }

    const std::uint32_t block_mask = block_count - 1;
    unsigned log2_block_count = 0;
    while ((1u << log2_block_count) != block_count) ++log2_block_count;
    const std::uint32_t max_coefficients = block_count * 64;
    std::uint32_t position = block_mask;
    std::uint32_t run = 4;
    std::uint32_t level = 2;
    for (;;) {
        const auto left = bits.bits_left();
        if (!left || (left < 32 && bits.show(static_cast<unsigned>(left)) == 0)) break;
        if (!decode_codeword(bits, kRunCodebook[std::min<std::uint32_t>(run, 15)], run))
            return fail(error, "truncated AC run");
        if (run >= max_coefficients || position > max_coefficients - run - 1)
            return fail(error, "AC run exceeds coefficient plane");
        position += run + 1;
        if (!decode_codeword(bits, kLevelCodebook[std::min<std::uint32_t>(level, 9)], level))
            return fail(error, "truncated AC level");
        ++level;
        std::uint32_t negative = 0;
        if (!bits.get(1, negative)) return fail(error, "truncated AC sign");
        const auto coefficient = negative ? -static_cast<std::int32_t>(level) :
                                            static_cast<std::int32_t>(level);
        const auto block = position & block_mask;
        const auto scan_index = position >> log2_block_count;
        output[block * 64 + kProgressiveScan[scan_index]] = coefficient;
    }
    return true;
}

}  // namespace

bool parse_frame(const std::uint8_t* data, std::size_t size,
                 std::uint16_t expected_width, std::uint16_t expected_height,
                 Frame& output, std::string& error) {
    output = {};
    error.clear();
    if (!data || size < 28 || size > 128u * 1024u * 1024u)
        return fail(error, "frame size is outside the supported range");
    if (read_be32(data) != size || data[4] != 'i' || data[5] != 'c' ||
        data[6] != 'p' || data[7] != 'f')
        return fail(error, "frame size/signature mismatch");

    const auto* header = data + 8;
    const auto header_size = read_be16(header);
    if (header_size < 20 || header_size > size - 8)
        return fail(error, "invalid frame header size");
    if (read_be16(header + 2) > 1) return fail(error, "unsupported frame header version");
    output.width = read_be16(header + 8);
    output.height = read_be16(header + 10);
    if (!output.width || !output.height || (output.width & 1) ||
        (expected_width && output.width != expected_width) ||
        (expected_height && output.height != expected_height))
        return fail(error, "frame dimensions do not match progressive 4:2:2 caps");
    if ((header[12] & 0xc0) != 0x80) return fail(error, "only ProRes 4:2:2 is supported");
    if (((header[12] >> 2) & 3) != 0) return fail(error, "interlaced ProRes is unsupported");
    if ((header[17] & 0x0f) != 0) return fail(error, "alpha ProRes is unsupported");
    output.color_primaries = header[14];
    output.transfer_characteristic = header[15];
    output.matrix_coefficients = header[16];

    const auto matrix_flags = header[19];
    std::size_t matrix_offset = 20;
    if (matrix_flags & 2) {
        if (matrix_offset + 64 > header_size) return fail(error, "truncated luma quant matrix");
        std::copy_n(header + matrix_offset, 64, output.luma_quant_matrix.begin());
        matrix_offset += 64;
    } else {
        output.luma_quant_matrix.fill(4);
    }
    if (matrix_flags & 1) {
        if (matrix_offset + 64 > header_size) return fail(error, "truncated chroma quant matrix");
        std::copy_n(header + matrix_offset, 64, output.chroma_quant_matrix.begin());
    } else {
        output.chroma_quant_matrix = output.luma_quant_matrix;
    }

    const std::size_t picture_offset = 8 + header_size;
    if (picture_offset + 8 > size) return fail(error, "missing picture header");
    const auto* picture = data + picture_offset;
    const auto picture_header_size = picture[0] >> 3;
    const auto picture_size = read_be32(picture + 1);
    if (picture_header_size < 8 || picture_header_size > size - picture_offset)
        return fail(error, "invalid picture header size");
    if (picture_size > size - picture_offset ||
        picture_size < static_cast<std::uint32_t>(picture_header_size))
        return fail(error, "invalid picture data size");
    if (picture_offset + picture_size != size)
        return fail(error, "progressive frame must contain exactly one complete picture");
    const unsigned log2_slice_width = picture[7] >> 4;
    const unsigned log2_slice_height = picture[7] & 15;
    if (log2_slice_width > 3 || log2_slice_height)
        return fail(error, "unsupported slice dimensions");
    const std::uint16_t nominal_slice_width = static_cast<std::uint16_t>(1u << log2_slice_width);
    output.mb_width = static_cast<std::uint16_t>((output.width + 15) >> 4);
    output.mb_height = static_cast<std::uint16_t>((output.height + 15) >> 4);
    const auto remainder = output.mb_width & (nominal_slice_width - 1);
    const auto slice_count = static_cast<std::size_t>(output.mb_height) *
        ((output.mb_width >> log2_slice_width) +
         population_count(remainder));
    if (!slice_count || picture_header_size + slice_count * 2 > picture_size)
        return fail(error, "slice table exceeds picture");

    const auto* index = picture + picture_header_size;
    std::size_t slice_offset = picture_offset + picture_header_size + slice_count * 2;
    std::uint16_t mb_x = 0;
    std::uint16_t mb_y = 0;
    std::uint16_t mb_count = nominal_slice_width;
    output.slices.reserve(slice_count);
    for (std::size_t i = 0; i < slice_count; ++i) {
        const auto slice_size = read_be16(index + i * 2);
        while (output.mb_width - mb_x < mb_count) mb_count >>= 1;
        if (!mb_count || slice_size < 6 || slice_offset > size || slice_size > size - slice_offset)
            return fail(error, "invalid slice size or macroblock coverage");
        const auto* slice_data = data + slice_offset;
        const auto slice_header_size = slice_data[0] >> 3;
        if (slice_header_size < 6 || slice_header_size > slice_size)
            return fail(error, "invalid slice header size");
        const auto y_size = read_be16(slice_data + 2);
        const auto u_size = read_be16(slice_data + 4);
        std::uint32_t v_size = 0;
        if (slice_header_size > 7) {
            v_size = read_be16(slice_data + 6);
        } else {
            if (slice_header_size + y_size + u_size > slice_size)
                return fail(error, "slice plane sizes exceed slice");
            v_size = slice_size - slice_header_size - y_size - u_size;
        }
        const std::uint64_t plane_total = static_cast<std::uint64_t>(slice_header_size) +
            y_size + u_size + v_size;
        if (!y_size || plane_total != slice_size)
            return fail(error, "slice has invalid plane sizes or unexpected alpha payload");

        Slice slice{};
        slice.offset = static_cast<std::uint32_t>(slice_offset);
        slice.size = slice_size;
        slice.mb_x = mb_x;
        slice.mb_y = mb_y;
        slice.mb_count = mb_count;
        slice.quant_index = std::clamp<std::uint8_t>(slice_data[1], 1, 224);
        auto plane_offset = slice_offset + slice_header_size;
        slice.planes[0] = {static_cast<std::uint32_t>(plane_offset), y_size};
        plane_offset += y_size;
        slice.planes[1] = {static_cast<std::uint32_t>(plane_offset), u_size};
        plane_offset += u_size;
        slice.planes[2] = {static_cast<std::uint32_t>(plane_offset), v_size};
        output.slices.push_back(slice);
        slice_offset += slice_size;

        mb_x = static_cast<std::uint16_t>(mb_x + mb_count);
        if (mb_x == output.mb_width) {
            mb_x = 0;
            ++mb_y;
            mb_count = nominal_slice_width;
        }
    }
    if (mb_x || mb_y != output.mb_height || slice_offset != picture_offset + picture_size)
        return fail(error, "slice table does not cover the picture exactly");
    return true;
}

void make_coefficient_jobs(const Frame& frame,
                           std::vector<CoefficientJob>& jobs,
                           std::uint32_t& coefficient_count) {
    jobs.clear();
    coefficient_count = 0;
    jobs.reserve(frame.slices.size() * 3);
    for (const auto& slice : frame.slices) {
        for (unsigned component = 0; component < 3; ++component) {
            const auto& plane = slice.planes[component];
            const auto block_count = static_cast<std::uint32_t>(slice.mb_count) *
                                     (component == 0 ? 4u : 2u);
            jobs.push_back({plane.offset, plane.size, block_count, coefficient_count});
            coefficient_count += block_count * 64;
        }
    }
}

bool make_coefficient_reference(const std::uint8_t* data, std::size_t size,
                                const Frame& frame,
                                std::vector<CoefficientJob>& jobs,
                                std::vector<std::int32_t>& coefficients,
                                std::string& error) {
    std::uint32_t coefficient_count = 0;
    make_coefficient_jobs(frame, jobs, coefficient_count);
    coefficients.assign(coefficient_count, 0);
    error.clear();
    for (std::size_t job_index = 0; job_index < jobs.size(); ++job_index) {
        const auto& job = jobs[job_index];
        const auto slice_index = job_index / 3;
        const auto component = static_cast<unsigned>(job_index % 3);
        if (job.data_offset > size || job.data_size > size - job.data_offset)
            return fail(error, "coefficient job is outside the frame");
        {
            std::ostringstream context;
            context << "slice " << slice_index << " component " << component << ": ";
            std::string detail;
            if (!decode_plane(data + job.data_offset, job.data_size, job.block_count,
                              coefficients.data() + job.output_offset, detail))
                return fail(error, context.str() + detail);
        }
    }
    return true;
}

void make_idct_jobs(const Frame& frame,
                    const std::vector<CoefficientJob>& coefficient_jobs,
                    std::vector<IdctBlockJob>& idct_jobs) {
    idct_jobs.clear();
    if (coefficient_jobs.size() != frame.slices.size() * 3) return;
    std::size_t block_total = 0;
    for (const auto& job : coefficient_jobs) block_total += job.block_count;
    idct_jobs.reserve(block_total);
    std::size_t job_index = 0;
    for (const auto& slice : frame.slices) {
        const auto quant_scale = slice.quant_index > 128 ?
            static_cast<std::uint32_t>(slice.quant_index - 96) * 4 : slice.quant_index;
        for (std::uint32_t component = 0; component < 3; ++component, ++job_index) {
            const auto& coefficient_job = coefficient_jobs[job_index];
            const auto chroma_shift = component ? 1u : 0u;
            const auto base_x = static_cast<std::uint32_t>(slice.mb_x) << (4 - chroma_shift);
            const auto base_y = static_cast<std::uint32_t>(slice.mb_y) << 4;
            for (std::uint32_t block = 0; block < coefficient_job.block_count; ++block) {
                std::uint32_t block_x = 0;
                std::uint32_t block_y = 0;
                if (!component) {
                    block_x = ((block & ~2u) + 1u) >> 1;
                    block_y = (block >> 1) & 1;
                } else {
                    block_x = (block & ~1u) >> 1;
                    block_y = block & 1;
                }
                idct_jobs.push_back({
                    coefficient_job.output_offset + block * 64,
                    base_x + block_x * 8,
                    base_y + block_y * 8,
                    component,
                    quant_scale,
                    component ? 64u : 0u,
                });
            }
        }
    }
}

}  // namespace prores
