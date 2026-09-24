/*
 * ProRes frame/slice parser used by the native D3D11 decoder.
 *
 * The bitstream rules and codebooks implemented by the companion .cpp are
 * derived from FFmpeg's LGPL-2.1-or-later libavcodec/proresdec.c.  Keeping the
 * parser independent from libavcodec makes accidental CPU decode fallback in
 * proresd3d11dec impossible.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace prores {

struct Plane {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct Slice {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint16_t mb_x = 0;
    std::uint16_t mb_y = 0;
    std::uint16_t mb_count = 0;
    std::uint8_t quant_index = 0;
    std::array<Plane, 4> planes{};
};

struct Frame {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t mb_width = 0;
    std::uint16_t mb_height = 0;
    std::uint8_t chroma_shift = 1;
    std::uint8_t bit_depth = 10;
    // 0: opaque, 1: 8-bit alpha, 2: 16-bit alpha.
    std::uint8_t alpha_info = 0;
    std::uint8_t color_primaries = 0;
    std::uint8_t transfer_characteristic = 0;
    std::uint8_t matrix_coefficients = 0;
    std::array<std::uint8_t, 64> luma_quant_matrix{};
    std::array<std::uint8_t, 64> chroma_quant_matrix{};
    std::vector<Slice> slices;
};

// One complete progressive ProRes frame. Bit depth comes from the container
// FourCC; chroma sampling and alpha depth come from the frame header. The
// Callers must opt in to alpha so legacy alpha-free checks remain explicit.
bool parse_frame(const std::uint8_t* data, std::size_t size,
                 std::uint16_t expected_width, std::uint16_t expected_height,
                 Frame& output, std::string& error, std::uint8_t bit_depth = 10,
                 bool allow_alpha = false);

struct CoefficientJob {
    std::uint32_t data_offset = 0;
    std::uint32_t data_size = 0;
    std::uint32_t block_count = 0;
    std::uint32_t output_offset = 0;
};

struct IdctBlockJob {
    std::uint32_t coefficient_offset = 0;
    std::uint32_t destination_x = 0;
    std::uint32_t destination_y = 0;
    std::uint32_t component = 0;
    std::uint32_t quant_scale = 0;
    std::uint32_t matrix_offset = 0;
};

void make_coefficient_jobs(const Frame& frame,
                           std::vector<CoefficientJob>& jobs,
                           std::uint32_t& coefficient_count);

// Emits one luma/U/V job per slice and a tightly packed coefficient array.
// Coefficients are in natural 8x8 row-major order after inverse scan.
bool make_coefficient_reference(const std::uint8_t* data, std::size_t size,
                                const Frame& frame,
                                std::vector<CoefficientJob>& jobs,
                                std::vector<std::int32_t>& coefficients,
                                std::string& error);

void make_idct_jobs(const Frame& frame,
                    const std::vector<CoefficientJob>& coefficient_jobs,
                    std::vector<IdctBlockJob>& idct_jobs);

}  // namespace prores
