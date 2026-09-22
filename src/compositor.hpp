// BT.709 limited の検証専用オフスクリーン合成。表示・HDR・色管理の実装ではない。
#include <d3dcompiler.h>
struct Compositor {
    ID3D11Device* dev;ID3D11DeviceContext* ctx;int layers;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11BlendState> blend;
    std::vector<ComPtr<ID3D11ShaderResourceView>> views;
    int width=0,height=0;
    bool verified=false;
    Compositor(ID3D11Device* d,ID3D11DeviceContext* c,int n):dev(d),ctx(c),layers(n) {
        const char* code=R"(
cbuffer Params : register(b0) {uint width;uint height;uint depth;uint chroma;uint hasAlpha;float opacity;float2 padding;};
Texture2D<float> Y : register(t0);Texture2D<float> U : register(t1);Texture2D<float> V : register(t2);Texture2D<float> A : register(t3);
float4 vert(uint id:SV_VertexID):SV_POSITION {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
float4 pixel(float4 position:SV_POSITION):SV_TARGET {
 int2 p=int2(position.xy),cp=int2(p.x>>chroma,p.y);float scale=1u<<(depth-8);
 float y=(Y.Load(int3(p,0))*65535-16*scale)/(219*scale);
 float u=(U.Load(int3(cp,0))*65535-128*scale)/(224*scale);
 float v=(V.Load(int3(cp,0))*65535-128*scale)/(224*scale);
 float a=hasAlpha?A.Load(int3(p,0))*65535/((1u<<depth)-1):1;
 return float4(y+1.5748*v,y-0.187324*u-0.468124*v,y+1.8556*u,a*opacity);
})";
        ComPtr<ID3DBlob> v,p,err;
        auto compile=[&](const char* entry,const char* profile,ID3DBlob** out){auto r=D3DCompile(code,std::strlen(code),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,out,&err);if(FAILED(r)&&err)std::cerr.write(static_cast<char*>(err->GetBufferPointer()),err->GetBufferSize());hrcheck(r);};
        compile("vert","vs_5_0",&v);compile("pixel","ps_5_0",&p);hrcheck(dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));hrcheck(dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps));
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hrcheck(dev->CreateBuffer(&bd,nullptr,&cb));
        D3D11_BLEND_DESC b{};auto& t=b.RenderTarget[0];t.BlendEnable=TRUE;t.SrcBlend=D3D11_BLEND_SRC_ALPHA;t.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;t.BlendOp=D3D11_BLEND_OP_ADD;t.SrcBlendAlpha=D3D11_BLEND_ONE;t.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;hrcheck(dev->CreateBlendState(&b,&blend));
    }
    void render(const std::vector<ComPtr<ID3D11Texture2D>>& planes,AVPixelFormat format,int w,int h) {
        const auto* desc=av_pix_fmt_desc_get(format);if(!desc||desc->log2_chroma_h!=0)throw std::runtime_error("compositor supports planar 422/444 only");
        if(width!=w||height!=h||views.empty()) {
            width=w;height=h;views.clear();
            for(auto& plane:planes){ComPtr<ID3D11ShaderResourceView> view;hrcheck(dev->CreateShaderResourceView(plane.Get(),nullptr,&view));views.push_back(view);}
            D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;hrcheck(dev->CreateTexture2D(&td,nullptr,&target));hrcheck(dev->CreateRenderTargetView(target.Get(),nullptr,&rtv));
        }
        std::array<ID3D11ShaderResourceView*,4> resources{};for(size_t i=0;i<views.size();i++)resources[i]=views[i].Get();
        auto* out=rtv.Get();auto* constants=cb.Get();float clear[4]={0,0,0,0};ctx->ClearRenderTargetView(out,clear);ctx->OMSetRenderTargets(1,&out,nullptr);ctx->OMSetBlendState(blend.Get(),nullptr,0xffffffff);
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(w),static_cast<float>(h),0,1};ctx->RSSetViewports(1,&viewport);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->PSSetShaderResources(0,4,resources.data());ctx->PSSetConstantBuffers(0,1,&constants);
        struct Params{UINT w,h,depth,chroma,alpha;float opacity,pad[2];};
        for(int i=0;i<layers;i++){Params p{static_cast<UINT>(w),static_cast<UINT>(h),static_cast<UINT>(desc->comp[0].depth),desc->log2_chroma_w,planes.size()==4?1u:0u,i?0.5f:1.f,{0,0}};ctx->UpdateSubresource(cb.Get(),0,nullptr,&p,0,0);ctx->Draw(3,0);}
        resources.fill(nullptr);ctx->PSSetShaderResources(0,4,resources.data());ctx->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void verify(AVFrame* reference) {
        if(verified)return;
        D3D11_TEXTURE2D_DESC d;target->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;hrcheck(dev->CreateTexture2D(&d,nullptr,&staging));ctx->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map;hrcheck(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
        auto half=[](uint16_t x){int e=(x>>10)&31,m=x&1023;double v=e?std::ldexp(1.+m/1024.,e-15):std::ldexp(m/1024.,-14);return x&32768?-v:v;};
        auto* desc=av_pix_fmt_desc_get(static_cast<AVPixelFormat>(reference->format));double scale=1<<(desc->comp[0].depth-8),peak=(1<<desc->comp[0].depth)-1,worst=0;
        for(int gy=0;gy<17;gy++)for(int gx=0;gx<17;gx++) {
            int x=gx*(width-1)/16,y=gy*(height-1)/16;
            auto sample=[&](int p){int px=p==1||p==2?x>>desc->log2_chroma_w:x;return reinterpret_cast<const uint16_t*>(reference->data[p]+y*reference->linesize[p])[px];};
            double Y=(sample(0)-16*scale)/(219*scale),U=(sample(1)-128*scale)/(224*scale),V=(sample(2)-128*scale)/(224*scale),a=desc->nb_components==4?sample(3)/peak:1.;
            double rgb[]={Y+1.5748*V,Y-.187324*U-.468124*V,Y+1.8556*U},expected[4]={0,0,0,0};
            for(int layer=0;layer<layers;layer++){double alpha=a*(layer?.5:1.);for(int p=0;p<3;p++)expected[p]=rgb[p]*alpha+expected[p]*(1-alpha);expected[3]=alpha+expected[3]*(1-alpha);}
            auto* actual=reinterpret_cast<uint16_t*>(static_cast<char*>(map.pData)+y*map.RowPitch)+x*4;
            for(int p=0;p<4;p++)worst=std::max(worst,std::abs(expected[p]-half(actual[p])));
        }
        ctx->Unmap(staging.Get(),0);if(worst>.01)throw std::runtime_error("composition sample error="+std::to_string(worst));
        verified=true;std::cerr<<"composition_first_frame_samples=289 max_abs="<<worst<<" tolerance=0.01\n";
    }
};
