// ProRes 8/16-bit alpha entropy decoder. One invocation owns one complete
// slice, so RLE state is sequential while slices run independently on the GPU.
// Bitstream behavior follows FFmpeg n8.1 libavcodec/proresdec.c.
// SPDX-License-Identifier: LGPL-2.1-or-later

ByteAddressBuffer frame_data : register(t0);

struct AlphaJob {
    uint data_offset;
    uint data_size;
    uint mb_x;
    uint mb_y;
    uint mb_count;
    uint field_layout; // low byte: row stride; next byte: field parity
};
StructuredBuffer<AlphaJob> jobs : register(t1);
RWTexture2D<uint> alpha_plane : register(u0);
RWStructuredBuffer<uint> errors : register(u1);

cbuffer Parameters : register(b0) {
    uint job_count;
    uint width;
    uint height;
    uint alpha_info_and_depth;
    uint error_base;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

struct BitReader {
    uint offset;
    uint bit_size;
    uint position;
    uint failed;
};

uint get_bits(inout BitReader bits, uint count) {
    if (count > bits.bit_size - min(bits.position, bits.bit_size)) {
        bits.failed = 1;
        return 0;
    }
    uint value = 0;
    for (uint i = 0; i < count; ++i) {
        uint byte_offset = bits.offset + (bits.position >> 3);
        uint byte_value = frame_data.Load(byte_offset & ~3u);
        byte_value = (byte_value >> ((byte_offset & 3u) * 8)) & 255u;
        value = (value << 1) | ((byte_value >> (7u - (bits.position & 7u))) & 1u);
        ++bits.position;
    }
    return value;
}

uint expanded_alpha(uint value, uint alpha_info) {
    // Keep the source's 16 bits; expand 8 bits to the full UNORM range.
    return alpha_info == 2u ? value : ((value << 8) | value);
}

void write_alpha(uint position, uint sample, AlphaJob job) {
    uint row_width = job.mb_count * 16u;
    uint x = job.mb_x * 16u + position % row_width;
    uint y = (job.mb_y * 16u + position / row_width) * (job.field_layout & 255u) +
        ((job.field_layout >> 8) & 255u);
    if (x < width && y < height)
        alpha_plane[uint2(x, y)] = sample;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint job_index = dispatch_id.x;
    if (job_index >= job_count) return;
    AlphaJob job = jobs[job_index];
    uint alpha_info = alpha_info_and_depth & 255u;
    uint source_bits = alpha_info == 2 ? 16u : 8u;
    uint mask = (1u << source_bits) - 1u;
    uint sample_count = job.mb_count * 256u;
    BitReader bits = {job.data_offset, job.data_size * 8u, 0u, 0u};
    uint position = 0;
    uint previous = mask;
    while (position < sample_count && !bits.failed) {
        while (position < sample_count && !bits.failed) {
            uint absolute = get_bits(bits, 1);
            int delta;
            if (absolute) {
                delta = int(get_bits(bits, source_bits));
            } else {
                uint code = get_bits(bits, source_bits == 16 ? 7u : 4u);
                delta = int((code + 2u) >> 1);
                if (code & 1u) delta = -delta;
            }
            if (bits.failed) break;
            previous = (previous + uint(delta)) & mask;
            write_alpha(position++, expanded_alpha(previous, alpha_info), job);
            if (position >= sample_count) break;
            if (bits.position >= bits.bit_size || get_bits(bits, 1) == 0) break;
        }
        if (position >= sample_count || bits.failed) break;
        uint repeat = get_bits(bits, 4);
        if (repeat == 0) repeat = get_bits(bits, 11);
        if (bits.failed || repeat > sample_count - position) {
            bits.failed = 1;
            break;
        }
        uint sample = expanded_alpha(previous, alpha_info);
        [loop] for (uint i = 0; i < repeat; ++i)
            write_alpha(position++, sample, job);
    }
    errors[error_base + job_index] = bits.failed || position != sample_count;
}
