// I422_10LE D3D11Memory (three R16_UNORM textures) -> full-range RGB10A2.
// Read the actual low-10-bit integer code before applying the BT.709 matrix.
Texture2D<float> luma : register(t0);
Texture2D<float> cb_plane : register(t1);
Texture2D<float> cr_plane : register(t2);
RWTexture2D<unorm float4> rgb_output : register(u0);

cbuffer Parameters : register(b0) {
    uint output_width;
    uint output_height;
    uint2 reserved;
};

float code_at(Texture2D<float> plane, int2 position) {
    return round(plane.Load(int3(position, 0)) * 65535.0);
}

float centered_chroma(Texture2D<float> plane, uint x, uint y) {
    int max_x = int(output_width / 2) - 1;
    float location = (float(x) - 0.5) * 0.5;
    int low = int(floor(location));
    float fraction = location - floor(location);
    int a = clamp(low, 0, max_x);
    int b = clamp(low + 1, 0, max_x);
    return lerp(code_at(plane, int2(a, y)), code_at(plane, int2(b, y)), fraction);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= output_width || id.y >= output_height) return;
    float y_code = code_at(luma, int2(id.xy));
    float cb_code = centered_chroma(cb_plane, id.x, id.y);
    float cr_code = centered_chroma(cr_plane, id.x, id.y);
    float yy = (y_code - 64.0) / 876.0;
    float cb = (cb_code - 512.0) / 896.0;
    float cr = (cr_code - 512.0) / 896.0;
    float3 value = saturate(float3(
        yy + 1.5748 * cr,
        yy - 0.187324 * cb - 0.468124 * cr,
        yy + 1.8556 * cb));
    rgb_output[id.xy] = float4(value, 1.0);
}
