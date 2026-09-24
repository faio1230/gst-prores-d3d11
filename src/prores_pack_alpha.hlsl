// Pack the four GPU-only ProRes components into Gst AYUV64 (A,Y,U,V).
// Every component uses the full 16-bit UNORM range expected by Gst AYUV64.
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
    uint bit_depth;
};

uint expand_yuv(float normalized) {
    uint code = uint(round(normalized * 65535.0));
    return bit_depth == 10u ? ((code << 6) | (code >> 4)) :
                              ((code << 4) | (code >> 8));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    int2 y_position = int2(id.xy);
    int2 chroma_position = int2(id.x >> chroma_shift, id.y);
    uint y = expand_yuv(y_plane.Load(int3(y_position, 0)));
    uint u = expand_yuv(u_plane.Load(int3(chroma_position, 0)));
    uint v = expand_yuv(v_plane.Load(int3(chroma_position, 0)));
    uint a = a_plane.Load(int3(y_position, 0));
    ayuv_output[id.xy] = float4(a, y, u, v) / 65535.0;
}
