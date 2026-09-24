// ProRes 4:2:2/4:4:4 entropy decoder and progressive/interlaced inverse scan.
// Derived from FFmpeg's LGPL-2.1-or-later proresdec.c/prores_vld.comp.glsl.
// SPDX-License-Identifier: LGPL-2.1-or-later

ByteAddressBuffer frame_data : register(t0);

struct CoefficientJob {
    uint data_offset;
    uint data_size;
    uint block_count;
    uint output_offset;
    uint interlaced_scan;
};

StructuredBuffer<CoefficientJob> jobs : register(t1);
RWStructuredBuffer<int> coefficients : register(u0);
RWStructuredBuffer<uint> errors : register(u1);

cbuffer Parameters : register(b0) {
    uint job_count;
    uint coefficient_count;
    uint reserved0;
    uint reserved1;
};

static const uint progressive_scan[64] = {
     0,  1,  8,  9,  2,  3, 10, 11,
    16, 17, 24, 25, 18, 19, 26, 27,
     4,  5, 12, 20, 13,  6,  7, 14,
    21, 28, 29, 22, 15, 23, 30, 31,
    32, 33, 40, 48, 41, 34, 35, 42,
    49, 56, 57, 50, 43, 36, 37, 44,
    51, 58, 59, 52, 45, 38, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};
static const uint interlaced_scan[64] = {
     0,  8,  1,  9, 16, 24, 17, 25,
     2, 10,  3, 11, 18, 26, 19, 27,
    32, 40, 33, 34, 41, 48, 56, 49,
    42, 35, 43, 50, 57, 58, 51, 59,
     4, 12,  5,  6, 13, 20, 28, 21,
    14,  7, 15, 22, 29, 36, 44, 37,
    30, 23, 31, 38, 45, 52, 60, 53,
    46, 39, 47, 54, 61, 62, 55, 63,
};

static const uint dc_codebook[7] = {
    0x04, 0x28, 0x28, 0x4d, 0x4d, 0x70, 0x70,
};
static const uint run_codebook[16] = {
    0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29,
    0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4c,
};
static const uint level_codebook[10] = {
    0x04, 0x0a, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4c,
};

struct BitReader {
    uint byte_offset;
    uint bit_size;
    uint position;
    uint failed;
};

uint load_byte(uint offset) {
    uint word = frame_data.Load(offset & ~3u);
    return (word >> ((offset & 3u) * 8)) & 255u;
}

uint byte_swap(uint value) {
    return ((value & 0x000000ffu) << 24) |
           ((value & 0x0000ff00u) << 8) |
           ((value & 0x00ff0000u) >> 8) |
           ((value & 0xff000000u) >> 24);
}

uint load_be32(uint offset) {
    uint aligned_offset = offset & ~3u;
    uint byte_shift = (offset & 3u) * 8;
    uint packed = frame_data.Load(aligned_offset) >> byte_shift;
    if (byte_shift != 0)
        packed |= frame_data.Load(aligned_offset + 4) << (32 - byte_shift);
    return byte_swap(packed);
}

uint show_bits(BitReader reader, uint count) {
    uint safe_count = min(count, 32u);
    uint remaining = reader.bit_size - min(reader.position, reader.bit_size);
    uint value = 0u;
    if (remaining >= 32 && safe_count != 0) {
        uint byte_offset = reader.byte_offset + (reader.position >> 3);
        uint bit_offset = reader.position & 7u;
        uint window = load_be32(byte_offset);
        if (bit_offset != 0)
            window = (window << bit_offset) |
                     (load_byte(byte_offset + 4) >> (8 - bit_offset));
        value = safe_count == 32 ? window : window >> (32 - safe_count);
    } else {
        // A short tail is uncommon and is kept bounded and explicit so no raw
        // buffer load can cross the padded packet allocation.
        [unroll] for (uint i = 0; i < 32; ++i) {
            if (i < safe_count) {
                value <<= 1;
                uint position = reader.position + i;
                if (position < reader.bit_size) {
                    uint byte_value = load_byte(reader.byte_offset + (position >> 3));
                    value |= (byte_value >> (7 - (position & 7))) & 1u;
                }
            }
        }
    }
    return value;
}

uint get_bits(inout BitReader reader, uint count) {
    if (count > 32 || count > reader.bit_size - min(reader.position, reader.bit_size)) {
        reader.failed = 1;
        return 0;
    }
    uint value = show_bits(reader, count);
    reader.position += count;
    return value;
}

void skip_bits(inout BitReader reader, uint count) {
    if (count > reader.bit_size - min(reader.position, reader.bit_size)) {
        reader.failed = 1;
        return;
    }
    reader.position += count;
}

uint decode_codeword(inout BitReader reader, uint codebook) {
    uint switch_bits = codebook & 3;
    uint rice_order = codebook >> 5;
    uint exp_order = (codebook >> 2) & 7;
    uint window = show_bits(reader, 32);
    int highest_bit = firstbithigh(window);
    uint q = highest_bit < 0 ? 32u : 31u - uint(highest_bit);
    if (q == 32) {
        reader.failed = 1;
        return 0;
    }
    if (q > switch_bits) {
        uint count = exp_order - switch_bits + q * 2;
        if (count > 31) {
            reader.failed = 1;
            return 0;
        }
        return get_bits(reader, count) - (1u << exp_order) +
               ((switch_bits + 1) << rice_order);
    }
    if (rice_order) {
        skip_bits(reader, q + 1);
        return (q << rice_order) + get_bits(reader, rice_order);
    }
    skip_bits(reader, q + 1);
    return q;
}

int to_signed(uint value) {
    return int((value >> 1) ^ uint(-int(value & 1)));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint job_index = dispatch_id.x;
    if (job_index >= job_count) return;
    CoefficientJob job = jobs[job_index];
    if (job.block_count == 0 || job.block_count > 32 ||
        (job.block_count & (job.block_count - 1)) != 0 ||
        job.output_offset > coefficient_count ||
        job.block_count * 64 > coefficient_count - job.output_offset) {
        errors[job_index] = 1;
        return;
    }
    if (job.data_size == 0) return;

    BitReader reader;
    reader.byte_offset = job.data_offset;
    reader.bit_size = job.data_size * 8;
    reader.position = 0;
    reader.failed = 0;

    uint code = decode_codeword(reader, 0xb8);
    if (code > 65535u || reader.failed) {
        errors[job_index] = 2;
        return;
    }
    int previous_dc = to_signed(code);
    if (previous_dc < -32768 || previous_dc > 32767) reader.failed = 1;
    if (reader.failed) {
        errors[job_index] = 2;
        return;
    }
    coefficients[job.output_offset] = previous_dc;
    code = 5;
    int sign = 0;
    [loop] for (uint block = 1; block < job.block_count; ++block) {
        code = decode_codeword(reader, dc_codebook[min(code, 6)]);
        if (code > 131071u || reader.failed) {
            reader.failed = 1;
            break;
        }
        if (code != 0) sign ^= -int(code & 1);
        else sign = 0;
        previous_dc += (int((code + 1) >> 1) ^ sign) - sign;
        if (previous_dc < -32768 || previous_dc > 32767) reader.failed = 1;
        coefficients[job.output_offset + block * 64] = previous_dc;
    }

    uint block_mask = job.block_count - 1;
    uint log2_block_count = firstbitlow(job.block_count);
    uint max_coefficients = job.block_count * 64;
    uint position = block_mask;
    uint run = 4;
    uint level = 2;
    [loop] for (uint iteration = 0; iteration < max_coefficients; ++iteration) {
        uint left = reader.bit_size - min(reader.position, reader.bit_size);
        if (left == 0 || (left < 32 && show_bits(reader, left) == 0)) break;
        run = decode_codeword(reader, run_codebook[min(run, 15)]);
        // Equality would advance position beyond the last coefficient.
        if (run >= max_coefficients || position >= max_coefficients - run - 1) {
            reader.failed = 1;
            break;
        }
        position += run + 1;
        uint decoded_level = decode_codeword(reader, level_codebook[min(level, 9)]);
        if (decoded_level >= 0x7fffffffu || reader.failed) {
            reader.failed = 1;
            break;
        }
        level = decoded_level + 1;
        uint negative = get_bits(reader, 1);
        uint block = position & block_mask;
        uint scan_index = position >> log2_block_count;
        int value = negative ? -int(level) : int(level);
        uint natural_index = job.interlaced_scan != 0 ?
            interlaced_scan[scan_index] : progressive_scan[scan_index];
        coefficients[job.output_offset + block * 64 + natural_index] = value;
        if (reader.failed) break;
    }
    if (reader.failed) errors[job_index] = 2;
}
