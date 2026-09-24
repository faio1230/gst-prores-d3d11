// ProRes inverse quantization, inverse DCT, rounding and 10/12-bit clipping.
// The separable transform is written directly here so the validation path has
// an independent, readable CPU formula to compare against.

StructuredBuffer<int> coefficients : register(t0);

struct IdctBlockJob {
    uint coefficient_offset;
    uint destination_x;
    uint destination_y;
    uint component;
    uint quant_scale;
    uint matrix_offset;
};

StructuredBuffer<IdctBlockJob> blocks : register(t1);
StructuredBuffer<uint> quant_matrices : register(t2);
#ifdef PRORES_OUTPUT_UNORM
RWTexture2D<unorm float> output_y : register(u0);
RWTexture2D<unorm float> output_u : register(u1);
RWTexture2D<unorm float> output_v : register(u2);
#else
RWTexture2D<uint> output_y : register(u0);
RWTexture2D<uint> output_u : register(u1);
RWTexture2D<uint> output_v : register(u2);
#endif

cbuffer Parameters : register(b0) {
    uint block_count;
    uint output_width;
    uint output_height;
    uint mode; // low bit: chroma horizontal shift; bits 8..15: output depth
};

static const float basis[64] = {
    0.7071067811865475,  0.7071067811865475,  0.7071067811865475,  0.7071067811865475,  0.7071067811865475,  0.7071067811865475,  0.7071067811865475,  0.7071067811865475,
    0.9807852804032304,  0.8314696123025452,  0.5555702330196023,  0.1950903220161283, -0.1950903220161282, -0.5555702330196020, -0.8314696123025453, -0.9807852804032304,
    0.9238795325112867,  0.3826834323650898, -0.3826834323650897, -0.9238795325112867, -0.9238795325112868, -0.3826834323650903,  0.3826834323650900,  0.9238795325112865,
    0.8314696123025452, -0.1950903220161282, -0.9807852804032304, -0.5555702330196022,  0.5555702330196018,  0.9807852804032304,  0.1950903220161288, -0.8314696123025451,
    0.7071067811865476, -0.7071067811865475, -0.7071067811865477,  0.7071067811865474,  0.7071067811865477, -0.7071067811865467, -0.7071067811865471,  0.7071067811865466,
    0.5555702330196023, -0.9807852804032304,  0.1950903220161283,  0.8314696123025455, -0.8314696123025451, -0.1950903220161280,  0.9807852804032307, -0.5555702330196015,
    0.3826834323650898, -0.9238795325112868,  0.9238795325112865, -0.3826834323650899, -0.3826834323650906,  0.9238795325112867, -0.9238795325112864,  0.3826834323650896,
    0.1950903220161283, -0.5555702330196022,  0.8314696123025455, -0.9807852804032307,  0.9807852804032304, -0.8314696123025450,  0.5555702330196015, -0.1950903220161286,
};

groupshared float dequantized[64];
groupshared float column_idct[64];

[numthreads(8, 8, 1)]
void main(uint3 group_id : SV_GroupID, uint3 thread_id : SV_GroupThreadID) {
    if (group_id.x >= block_count) return;
    IdctBlockJob job = blocks[group_id.x];
    uint x = thread_id.x;
    uint y = thread_id.y;
    uint index = y * 8 + x;
    float coefficient = float(coefficients[job.coefficient_offset + index]);
    float quant = float(quant_matrices[job.matrix_offset + index] * job.quant_scale);
    dequantized[index] = coefficient * quant;
    GroupMemoryBarrierWithGroupSync();

    // Separable 8x8 IDCT: 16 multiply-adds per output instead of evaluating
    // the 64-term 2D expression in every lane.
    float sum = 0.0;
    [unroll] for (uint v = 0; v < 8; ++v)
        sum += dequantized[v * 8 + x] * basis[v * 8 + y];
    column_idct[index] = sum;
    GroupMemoryBarrierWithGroupSync();
    sum = 0.0;
    [unroll] for (uint u = 0; u < 8; ++u)
        sum += column_idct[y * 8 + u] * basis[u * 8 + x];
    // The orthogonal 8x8 inverse transform contributes 1/4. The 10-bit
    // output additionally divides the 12-bit coefficient domain by four.
    uint bit_depth = (mode >> 8) & 255u;
    float scale = bit_depth == 12u ? 0.25 : 0.0625;
    float bias = bit_depth == 12u ? 2048.0 : 512.0;
    int maximum = bit_depth == 12u ? 4091 : 1019;
    int value = clamp(int(round(bias + sum * scale)), 4, maximum);
    uint2 destination = uint2(job.destination_x + x, job.destination_y + y);
    uint plane_width = job.component == 0 ? output_width : output_width >> (mode & 1u);
    if (destination.x >= plane_width || destination.y >= output_height) return;
#ifdef PRORES_OUTPUT_UNORM
    // GStreamer planar 10/12-bit formats store the code in the low bits of
    // the 16-bit container even though the D3D resource view is R16_UNORM.
    float normalized = float(value) / 65535.0;
    if (job.component == 0) output_y[destination] = normalized;
    else if (job.component == 1) output_u[destination] = normalized;
    else output_v[destination] = normalized;
#else
    if (job.component == 0) output_y[destination] = uint(value);
    else if (job.component == 1) output_u[destination] = uint(value);
    else output_v[destination] = uint(value);
#endif
}
