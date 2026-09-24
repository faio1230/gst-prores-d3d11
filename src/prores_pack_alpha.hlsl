// Pack the four GPU-only ProRes components into Gst AYUV64 (A,Y,U,V).
// Each integer code occupies the low bits of its 16-bit component.
// SPDX-License-Identifier: LGPL-2.1-or-later
Texture2D<float> y_plane : register(t0);
Texture2D<float> u_plane : register(t1);
Texture2D<float> v_plane : register(t2);
Texture2D<uint> a_plane : register(t3);
RWTexture2D<unorm float4> ayuv_output : register(u0);

cbuffer Parameters : register(b0) {
    uint width;
    uint height;
    uint chroma_shift;
    uint reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    int2 y_position = int2(id.xy);
    int2 chroma_position = int2(id.x >> chroma_shift, id.y);
    float y = y_plane.Load(int3(y_position, 0));
    float u = u_plane.Load(int3(chroma_position, 0));
    float v = v_plane.Load(int3(chroma_position, 0));
    uint a = a_plane.Load(int3(y_position, 0));
    ayuv_output[id.xy] = float4(float(a) / 65535.0, y, u, v);
}
