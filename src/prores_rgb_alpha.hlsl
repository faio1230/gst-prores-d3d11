// AYUV64 low-bit ProRes codes -> full-range RGBA64_LE with retained alpha.
// Only limited-range BT.709 input is negotiated by the owning element.
// SPDX-License-Identifier: LGPL-2.1-or-later
Texture2D<float4> ayuv_input : register(t0);
RWTexture2D<unorm float4> rgba_output : register(u0);

cbuffer Parameters : register(b0) {
    uint width;
    uint height;
    uint mode; // low bit: original chroma shift; bits 8..15: ProRes depth
    uint reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float4 code = round(ayuv_input.Load(int3(id.xy, 0)) * 65535.0);
    float2 chroma_code = code.ba;
    if (mode & 1u) {
        int max_x = int(width / 2) - 1;
        float location = (float(id.x) - 0.5) * 0.5;
        int low = int(floor(location));
        float fraction = location - floor(location);
        int a = clamp(low, 0, max_x);
        int b = clamp(low + 1, 0, max_x);
        float2 first = round(ayuv_input.Load(int3(a * 2, id.y, 0)).ba * 65535.0);
        float2 second = round(ayuv_input.Load(int3(b * 2, id.y, 0)).ba * 65535.0);
        chroma_code = lerp(first, second, fraction);
    }
    uint depth = (mode >> 8) & 255u;
    float depth_scale = depth == 12u ? 4.0 : 1.0;
    float yy = (code.g / depth_scale - 64.0) / 876.0;
    float cb = (chroma_code.x / depth_scale - 512.0) / 896.0;
    float cr = (chroma_code.y / depth_scale - 512.0) / 896.0;
    float3 value = saturate(float3(
        yy + 1.5748 * cr,
        yy - 0.187324 * cb - 0.468124 * cr,
        yy + 1.8556 * cb));
    float alpha = saturate(code.r / (depth == 12u ? 4095.0 : 1023.0));
    rgba_output[id.xy] = float4(value, alpha);
}
