#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
static double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double,std::milli>(b-a).count(); }
static void avcheck(int r) { if(r<0){ char msg[AV_ERROR_MAX_STRING_SIZE]; av_strerror(r,msg,sizeof(msg)); throw std::runtime_error(msg); } }
static void hrcheck(HRESULT r) { if(FAILED(r)) throw std::runtime_error("D3D11 HRESULT="+std::to_string(static_cast<unsigned long>(r))); }
static void vkcheck(VkResult r) { if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan result="+std::to_string(r)); }
static AVPixelFormat select_vk(AVCodecContext*, const AVPixelFormat* p) { for(;*p!=AV_PIX_FMT_NONE;++p) if(*p==AV_PIX_FMT_VULKAN) return *p; return AV_PIX_FMT_NONE; }
static double cpu_seconds() { FILETIME c,e,k,u; if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u)) throw std::runtime_error("GetProcessTimes"); ULARGE_INTEGER x{},y{}; x.LowPart=k.dwLowDateTime;x.HighPart=k.dwHighDateTime;y.LowPart=u.dwLowDateTime;y.HighPart=u.dwHighDateTime;return (x.QuadPart+y.QuadPart)*1e-7; }
static double percentile(std::vector<double> a,double p) { if(a.empty()) return 0;std::sort(a.begin(),a.end());return a[static_cast<size_t>(std::ceil(p*(a.size()-1)))]; }
#include "interop.hpp"

// 比較用のCPU経由経路。各プレーンをR16_UNORMへアップロードする。合成は任意。
struct D3DUpload {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Query> done;
    std::vector<ComPtr<ID3D11Texture2D>> tex;
    std::unique_ptr<Compositor> compositor;
    int width=0,height=0,format=-1;
    D3DUpload(int layers) {
        D3D_FEATURE_LEVEL level;
        hrcheck(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx));
        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0}; hrcheck(dev->CreateQuery(&q,&done));
        std::cerr<<"d3d_feature_level="<<std::hex<<level<<std::dec<<"\n";
        if(layers)compositor=std::make_unique<Compositor>(dev.Get(),ctx.Get(),layers);
    }
    void upload(AVFrame* f) {
        auto* desc=av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
        if(!desc || desc->comp[0].step!=2 || !(desc->flags&AV_PIX_FMT_FLAG_PLANAR)) throw std::runtime_error("16-bit planar only");
        int planes=av_pix_fmt_count_planes(static_cast<AVPixelFormat>(f->format));
        if(width&&(width!=f->width||height!=f->height||format!=f->format))throw std::runtime_error("upload dynamic format change unsupported");
        if(width!=f->width || height!=f->height || format!=f->format) {
            tex.clear();width=f->width;height=f->height;format=f->format;
            for(int p=0;p<planes;p++) {
                int shift=p==1||p==2 ? desc->log2_chroma_w:0;
                int sh=p==1||p==2 ? desc->log2_chroma_h:0;
                D3D11_TEXTURE2D_DESC d{}; d.Width=(width+(1<<shift)-1)>>shift; d.Height=(height+(1<<sh)-1)>>sh;
                d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R16_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> t;hrcheck(dev->CreateTexture2D(&d,nullptr,&t));tex.push_back(t);
            }
        }
        for(int p=0;p<planes;p++) ctx->UpdateSubresource(tex[p].Get(),0,nullptr,f->data[p],f->linesize[p],0);
        if(compositor){compositor->render(tex,static_cast<AVPixelFormat>(f->format),f->width,f->height);compositor->verify(f);}
        ctx->End(done.Get());ctx->Flush();
        auto start=Clock::now();
        for(;;) { auto r=ctx->GetData(done.Get(),nullptr,0,0);hrcheck(r);if(r==S_OK)break;if(ms(start,Clock::now())>10000)throw std::runtime_error("D3D11 timeout");SwitchToThread(); }
    }
};
struct Decoder {
    AVFormatContext* input=nullptr; AVCodecContext* codec=nullptr;
    AVBufferRef* device=nullptr; AVPacket* pkt=av_packet_alloc(); AVFrame* frame=av_frame_alloc(); AVFrame* sw=av_frame_alloc();
    ~Decoder(){av_frame_free(&sw);av_frame_free(&frame);av_packet_free(&pkt);avcodec_free_context(&codec);av_buffer_unref(&device);avformat_close_input(&input);}
};
int main(int argc,char**argv) try {
    if(argc<5) {std::cerr<<"prores_bench input cpu|vulkan|download|cpu-d3d11|download-d3d11|interop frames.csv loops [threads=0] [warmup=30] [raw-file] [frame-limit=0]\n";return 2;}
    std::string mode=argv[2];
    if(mode!="cpu"&&mode!="vulkan"&&mode!="download"&&mode!="cpu-d3d11"&&mode!="download-d3d11"&&mode!="interop") throw std::runtime_error("unknown mode");
    int loops=std::stoi(argv[4]),threads=argc>5?std::stoi(argv[5]):0,warmup=argc>6?std::stoi(argv[6]):30;
    char layerText[32]{};GetEnvironmentVariableA("PRORES_COMPOSE_LAYERS",layerText,sizeof(layerText));int layers=layerText[0]?std::stoi(layerText):0;if(layers<0||layers>64)throw std::runtime_error("invalid layers");
    if(layers&&mode!="cpu-d3d11"&&mode!="download-d3d11"&&mode!="interop")throw std::runtime_error("composition requires D3D11 output mode");
    std::ofstream dump;if(argc>7){dump.open(argv[7],std::ios::binary);if(!dump)throw std::runtime_error("cannot open raw output");}
    int limit=argc>8?std::stoi(argv[8]):0;if(limit<0)throw std::runtime_error("invalid limit");
    if(loops<1||threads<0||warmup<0) throw std::runtime_error("invalid numeric argument");
    bool gpu=mode=="vulkan"||mode=="download"||mode=="download-d3d11"||mode=="interop",download=mode=="download"||mode=="download-d3d11";
    av_log_set_level(AV_LOG_WARNING);
    auto start=Clock::now();double cpu0=cpu_seconds(); Decoder d;
    avcheck(avformat_open_input(&d.input,argv[1],nullptr,nullptr));avcheck(avformat_find_stream_info(d.input,nullptr));
    int stream=av_find_best_stream(d.input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);avcheck(stream);
    auto* st=d.input->streams[stream]; if(st->codecpar->codec_id!=AV_CODEC_ID_PRORES)throw std::runtime_error("ordinary ProRes input required");
    auto* dec=avcodec_find_decoder_by_name("prores");if(!dec)throw std::runtime_error("prores decoder missing");
    d.codec=avcodec_alloc_context3(dec);if(!d.codec||!d.frame||!d.sw||!d.pkt)throw std::bad_alloc();
    avcheck(avcodec_parameters_to_context(d.codec,st->codecpar));d.codec->thread_count=threads;
    auto demux_end=Clock::now();
    if(gpu){AVDictionary* opts=nullptr;if(mode=="interop")av_dict_set(&opts,"device_extensions","VK_KHR_win32_keyed_mutex",0);int r=av_hwdevice_ctx_create(&d.device,AV_HWDEVICE_TYPE_VULKAN,"0",opts,0);av_dict_free(&opts);avcheck(r);d.codec->hw_device_ctx=av_buffer_ref(d.device);d.codec->get_format=select_vk;}
    auto device_end=Clock::now();
    avcheck(avcodec_open2(d.codec,dec,nullptr)); auto open_end=Clock::now();
    std::unique_ptr<D3DUpload> upload;
    if(mode=="cpu-d3d11"||mode=="download-d3d11")upload=std::make_unique<D3DUpload>(layers);
    std::unique_ptr<VulkanD3D11> interop;if(mode=="interop")interop=std::make_unique<VulkanD3D11>(d.device,layers);
    std::ofstream csv(argv[3]);if(!csv)throw std::runtime_error("cannot open CSV");
    csv<<"epoch,frame,pts,receive_ms,delivery_ms,completed_interval_ms,steady\n";
    long long count=0,steady_count=0;double steady_ms=0,first_ms=0;
    std::vector<double> intervals,seeks;auto previous=Clock::now();
    auto* hw=gpu?reinterpret_cast<AVHWDeviceContext*>(d.device->data):nullptr;
    auto* vk=hw?static_cast<AVVulkanDeviceContext*>(hw->hwctx):nullptr;
    PFN_vkWaitSemaphores wait=vk?reinterpret_cast<PFN_vkWaitSemaphores>(vk->get_proc_addr(vk->inst,"vkWaitSemaphores")):nullptr;
    if(gpu&&!wait)throw std::runtime_error("vkWaitSemaphores unavailable");
    for(int epoch=0;epoch<loops;epoch++) {
        auto seek_start=Clock::now();
        if(epoch){ avcheck(av_seek_frame(d.input,stream,st->start_time==AV_NOPTS_VALUE?0:st->start_time,AVSEEK_FLAG_BACKWARD));avcodec_flush_buffers(d.codec); }
        int epoch_frames=0;
        auto receive=[&]() {
            for(;;) {
                if(limit&&count>=limit)return AVERROR_EOF;
                auto rstart=Clock::now();int ret=avcodec_receive_frame(d.codec,d.frame);auto rend=Clock::now();
                if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF)return ret;avcheck(ret);
                if(gpu&&d.frame->format!=AV_PIX_FMT_VULKAN)throw std::runtime_error("software fallback rejected");
                if(layers&&(d.frame->color_range!=AVCOL_RANGE_MPEG||d.frame->colorspace!=AVCOL_SPC_BT709))throw std::runtime_error("compositor requires explicit BT.709 limited metadata");
                if(gpu) {
                    auto* fc=reinterpret_cast<AVHWFramesContext*>(d.frame->hw_frames_ctx->data);
                    auto* vfc=static_cast<AVVulkanFramesContext*>(fc->hwctx);
                    auto* f=reinterpret_cast<AVVkFrame*>(d.frame->data[0]);
                    vfc->lock_frame(fc,f);
                    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};std::vector<VkSemaphore> sem;std::vector<uint64_t> vals;
                    for(int i=0;i<AV_NUM_DATA_POINTERS&&f->sem[i];i++){sem.push_back(f->sem[i]);vals.push_back(f->sem_value[i]);}
                    wi.semaphoreCount=static_cast<uint32_t>(sem.size());wi.pSemaphores=sem.data();wi.pValues=vals.data();
                    auto result=wait(vk->act_dev,&wi,10000000000ULL);vfc->unlock_frame(fc,f);vkcheck(result);
                }
                AVFrame* output=d.frame;
                if(download){av_frame_unref(d.sw);avcheck(av_hwframe_transfer_data(d.sw,d.frame,0));output=d.sw;}
                if(upload)upload->upload(output);
                if(interop){interop->copy(d.frame);interop->verify(d.frame);}
                if(dump.is_open()){
                    if(output->format==AV_PIX_FMT_VULKAN)throw std::runtime_error("raw output needs cpu/download mode");
                    auto* desc=av_pix_fmt_desc_get(static_cast<AVPixelFormat>(output->format));int planes=av_pix_fmt_count_planes(static_cast<AVPixelFormat>(output->format));
                    if(!desc||desc->comp[0].step!=2)throw std::runtime_error("raw dump needs 16-bit storage");
                    for(int p=0;p<planes;p++){int sx=p==1||p==2?desc->log2_chroma_w:0,sy=p==1||p==2?desc->log2_chroma_h:0;int w=(output->width+(1<<sx)-1)>>sx,h=(output->height+(1<<sy)-1)>>sy;for(int y=0;y<h;y++)dump.write(reinterpret_cast<char*>(output->data[p]+y*output->linesize[p]),w*2);}
                    if(!dump)throw std::runtime_error("raw write failure");
                }
                auto end=Clock::now();double dt=ms(previous,end);previous=end;
                if(!count)first_ms=ms(start,end);
                if(!epoch_frames&&epoch)seeks.push_back(ms(seek_start,end));
                bool steady=epoch_frames>=warmup;
                if(steady){steady_count++;steady_ms+=dt;intervals.push_back(dt);}
                csv<<epoch<<','<<epoch_frames<<','<<d.frame->best_effort_timestamp<<','<<ms(rstart,rend)<<','<<ms(rend,end)<<','<<dt<<','<<steady<<'\n';
                if(!count)std::cerr<<"frame="<<d.frame->width<<"x"<<d.frame->height<<" output="<<av_get_pix_fmt_name(static_cast<AVPixelFormat>(d.frame->format))<<" color_range="<<d.frame->color_range<<" colorspace="<<d.frame->colorspace<<"\n";
                count++;epoch_frames++;av_frame_unref(d.frame);
            }
        };
        int ret=AVERROR_EOF;
        while((!limit||count<limit)&&(ret=av_read_frame(d.input,d.pkt))>=0) {
            if(d.pkt->stream_index==stream){int sent=avcodec_send_packet(d.codec,d.pkt);if(sent==AVERROR(EAGAIN)){receive();sent=avcodec_send_packet(d.codec,d.pkt);}avcheck(sent);receive();}
            av_packet_unref(d.pkt);
        }
        if(ret<0&&ret!=AVERROR_EOF)avcheck(ret);
        if(!limit||count<limit){avcheck(avcodec_send_packet(d.codec,nullptr));receive();}
        if(!epoch_frames)throw std::runtime_error("no decoded frames");
        if(limit&&count>=limit)break;
    }
    auto end=Clock::now();PROCESS_MEMORY_COUNTERS_EX mem{};mem.cb=sizeof(mem);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem),sizeof(mem));
    SYSTEM_INFO si;GetSystemInfo(&si);double wall=ms(start,end),cpu=cpu_seconds()-cpu0;
    std::cout<<"{\"mode\":\""<<mode<<"\",\"compose_layers\":"<<layers<<",\"raw_dump\":"<<(dump.is_open()?"true":"false")<<",\"frames\":"<<count<<",\"threads\":"<<threads<<",\"warmup_per_epoch\":"<<warmup
    <<",\"demux_init_ms\":"<<ms(start,demux_end)<<",\"device_init_ms\":"<<ms(demux_end,device_end)<<",\"codec_open_ms\":"<<ms(device_end,open_end)
    <<",\"first_completed_ms\":"<<first_ms<<",\"wall_ms\":"<<wall<<",\"steady_frames\":"<<steady_count<<",\"steady_fps\":"<<(steady_ms?steady_count*1000/steady_ms:0)
    <<",\"interval_p50_ms\":"<<percentile(intervals,.50)<<",\"interval_p95_ms\":"<<percentile(intervals,.95)<<",\"interval_p99_ms\":"<<percentile(intervals,.99)
    <<",\"seek_first_p95_ms\":"<<percentile(seeks,.95)<<",\"seek_count\":"<<seeks.size()<<",\"cpu_seconds\":"<<cpu<<",\"cpu_machine_percent\":"<<cpu*100000/(wall*si.dwNumberOfProcessors)
    <<",\"peak_working_set_mib\":"<<mem.PeakWorkingSetSize/1048576.0<<",\"private_mib_end\":"<<mem.PrivateUsage/1048576.0<<"}\n";
    return 0;
} catch(const std::exception&e) {std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
