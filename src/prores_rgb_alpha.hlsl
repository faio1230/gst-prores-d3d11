// Standard AYUV64 UNORM -> full-range RGBA64_LE with retained alpha.
// Only limited-range BT.709 input is negotiated by the owning element.
// SPDX-License-Identifier: LGPL-2.1-or-later
Texture2D<float4> ayuv_input : register(t0);
RWTexture2D<unorm float4> rgba_output : register(u0);

cbuffer Parameters : register(b0) {
    uint width;
    uint height;
    uint mode; // unused for standard AYUV64
    uint reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float4 code = ayuv_input.Load(int3(id.xy, 0));
    // Gst AYUV64 limited range is the 8-bit legal interval left-shifted by 8.
    float yy = (code.g * 65535.0 - 4096.0) / 56064.0;
    float cb = (code.b * 65535.0 - 32768.0) / 57344.0;
    float cr = (code.a * 65535.0 - 32768.0) / 57344.0;
    float3 value = saturate(float3(
        yy + 1.5748 * cr,
        yy - 0.187324 * cb - 0.468124 * cr,
        yy + 1.8556 * cb));
    rgba_output[id.xy] = float4(value, code.r);
}
