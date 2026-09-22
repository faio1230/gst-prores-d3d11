// SM5で必要な基礎処理を検証する独立実装。ProResデコーダーそのものではない。
// ビット読み取りはuintのByteAddressBufferで8/16ビット格納を代替する。
ByteAddressBuffer bitstream : register(t0);
StructuredBuffer<float> coeffs : register(t1);
RWStructuredBuffer<float2> outputData : register(u0);
groupshared float block[64];
groupshared float columns[64];

uint read_bits(uint offset, uint n) {
    uint result = 0;
    for (uint i=0; i<n; i++) {
        uint pos=offset+i, byteIndex=pos>>3;
        uint word=bitstream.Load(byteIndex & ~3u);
        uint byteValue=(word >> ((byteIndex & 3u)*8)) & 255u;
        result=(result<<1) | ((byteValue >> (7-(pos&7))) & 1u);
    }
    return result;
}
float basis(uint k, uint x) {
    return (k==0 ? 0.7071067811865475 : 1.0) * cos(3.141592653589793*(2*x+1)*k/16.0);
}
[numthreads(8,8,1)]
void main(uint3 thread : SV_GroupThreadID, uint3 group : SV_GroupID) {
    uint x=thread.x, y=thread.y, index=y*8+x;
    block[index]=coeffs[group.x*64+index];
    GroupMemoryBarrierWithGroupSync();
    float v=0;
    [unroll] for(uint k=0;k<8;k++) v+=block[k*8+x]*basis(k,y);
    columns[index]=v*0.5;
    GroupMemoryBarrierWithGroupSync();
    v=0;
    [unroll] for(uint k=0;k<8;k++) v+=columns[y*8+k]*basis(k,x);
    uint offset=(group.x*61+index*17)%1000;
    uint n=1+index%16;
    outputData[group.x*64+index]=float2(v*0.5,read_bits(offset,n));
}
