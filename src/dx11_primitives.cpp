// 実機でSM5のビット読み取りと8x8 IDCTをCPU倍精度参照と照合する。
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static void check(HRESULT r){if(FAILED(r))throw std::runtime_error("HRESULT="+std::to_string(static_cast<unsigned long>(r)));}
int wmain(int argc,wchar_t**argv) try {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,requested,1,D3D11_SDK_VERSION,&dev,&level,&ctx));
    ComPtr<ID3DBlob> code,error;HRESULT r=D3DCompileFromFile(argc>1?argv[1]:L"src/dx11_primitives.hlsl",nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(r)&&error)std::cerr.write(static_cast<char*>(error->GetBufferPointer()),error->GetBufferSize());check(r);
    ComPtr<ID3D11ComputeShader> shader;check(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    constexpr UINT blocks=128,count=blocks*64;
    std::array<unsigned char,256> bytes{};for(UINT i=0;i<bytes.size();i++)bytes[i]=static_cast<unsigned char>((i*73+19)&255);
    std::vector<float> coeff(count);
    for(UINT b=0;b<blocks;b++)for(UINT i=0;i<64;i++)coeff[b*64+i]=b<64?(b==i?1024.f:0.f):static_cast<float>(static_cast<int>((b*173+i*59)%4096)-2048);
    ComPtr<ID3D11Buffer> bits,input,output,staging;
    D3D11_BUFFER_DESC d{};d.ByteWidth=static_cast<UINT>(bytes.size());d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA data{bytes.data(),0,0};check(dev->CreateBuffer(&d,&data,&bits));
    d.ByteWidth=count*sizeof(float);d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=sizeof(float);data.pSysMem=coeff.data();check(dev->CreateBuffer(&d,&data,&input));
    d.ByteWidth=count*sizeof(float)*2;d.StructureByteStride=sizeof(float)*2;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;check(dev->CreateBuffer(&d,nullptr,&output));
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;d.StructureByteStride=0;check(dev->CreateBuffer(&d,nullptr,&staging));
    ComPtr<ID3D11ShaderResourceView> bitView,inputView;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_TYPELESS;srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;srv.BufferEx.NumElements=static_cast<UINT>(bytes.size()/4);srv.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;check(dev->CreateShaderResourceView(bits.Get(),&srv,&bitView));
    check(dev->CreateShaderResourceView(input.Get(),nullptr,&inputView));
    ComPtr<ID3D11UnorderedAccessView> uav;check(dev->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    ID3D11ShaderResourceView* views[]={bitView.Get(),inputView.Get()};ID3D11UnorderedAccessView* outputs[]={uav.Get()};
    ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,2,views);ctx->CSSetUnorderedAccessViews(0,1,outputs,nullptr);ctx->Dispatch(blocks,1,1);
    ctx->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped;check(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    auto* gpu=static_cast<const float*>(mapped.pData);double worst=0,sum=0;UINT bit_errors=0;
    constexpr double pi=3.14159265358979323846;
    auto basis=[&](UINT k,UINT x){return (k==0?std::sqrt(.5):1.)*std::cos(pi*(2*x+1)*k/16.);};
    for(UINT b=0;b<blocks;b++)for(UINT y=0;y<8;y++)for(UINT x=0;x<8;x++) {
        double ref=0;for(UINT v=0;v<8;v++)for(UINT u=0;u<8;u++)ref+=coeff[b*64+v*8+u]*basis(u,x)*basis(v,y)*.25;
        UINT i=y*8+x;double diff=std::abs(gpu[(b*64+i)*2]-ref);worst=std::max(worst,diff);sum+=diff;
        UINT pos=(b*61+i*17)%1000,n=1+i%16,bits_ref=0;for(UINT j=0;j<n;j++)bits_ref=(bits_ref<<1)|((bytes[(pos+j)/8]>>(7-(pos+j)%8))&1);
        if(gpu[(b*64+i)*2+1]!=static_cast<float>(bits_ref))bit_errors++;
    }
    ctx->Unmap(staging.Get(),0);
    UINT support=0;check(dev->CheckFormatSupport(DXGI_FORMAT_R16_UINT,&support));
    bool passed=worst<0.02&&bit_errors==0;
    std::cout<<"{\"feature_level\":"<<level<<",\"shader_model\":\"cs_5_0\",\"blocks\":"<<blocks<<",\"bit_tests\":"<<count<<",\"bit_errors\":"<<bit_errors<<",\"idct_max_abs\":"<<worst<<",\"idct_mae\":"<<sum/count<<",\"r16_uint_typed_uav\":"<<((support&D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)?"true":"false")<<",\"passed\":"<<(passed?"true":"false")<<",\"full_prores_decoder\":false}\n";
    return passed?0:1;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
