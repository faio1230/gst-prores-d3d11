// 検証専用: FFmpeg Vulkan画像 -> 共有R16テクスチャ -> D3D11所有テクスチャ。
// 各プレーンにつきGPUコピー2回。フレーム単位で完了待ちする保守的な同期。
#include <array>
#include <cstring>
#include "compositor.hpp"

struct VulkanD3D11 {
    AVHWDeviceContext* hw;
    AVVulkanDeviceContext* vk;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Query> done;
    struct Plane {
        ComPtr<ID3D11Texture2D> shared, consumer;
        ComPtr<IDXGIKeyedMutex> mutex;
        VkImage image=VK_NULL_HANDLE;VkDeviceMemory mem=VK_NULL_HANDLE;
        uint32_t w=0,h=0;bool initialized=false;
    };
    std::vector<Plane> planes;
    std::unique_ptr<Compositor> compositor;
    VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer cmd=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;VkQueue queue=VK_NULL_HANDLE;
    uint32_t family=0;int width=0,height=0,format=-1;bool checked=false;
#define VK_FUNCTIONS(X) \
    X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceImageFormatProperties2) X(GetPhysicalDeviceMemoryProperties) \
    X(GetDeviceQueue) X(CreateCommandPool) X(AllocateCommandBuffers) X(CreateFence) X(CreateImage) \
    X(GetImageMemoryRequirements) X(GetMemoryWin32HandlePropertiesKHR) X(AllocateMemory) X(BindImageMemory) \
    X(ResetCommandBuffer) X(BeginCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(EndCommandBuffer) \
    X(ResetFences) X(QueueSubmit) X(WaitForFences) X(DeviceWaitIdle) X(DestroyImage) X(FreeMemory) X(DestroyFence) X(DestroyCommandPool)
#define DECLARE_FN(name) PFN_vk##name fn##name=nullptr;
    VK_FUNCTIONS(DECLARE_FN)
#undef DECLARE_FN
    explicit VulkanD3D11(AVBufferRef* device,int layers):hw(reinterpret_cast<AVHWDeviceContext*>(device->data)),vk(static_cast<AVVulkanDeviceContext*>(hw->hwctx)) {
#define LOAD_FN(name) fn##name=reinterpret_cast<PFN_vk##name>(vk->get_proc_addr(vk->inst,"vk" #name)); if(!fn##name)throw std::runtime_error("missing vk" #name);
        VK_FUNCTIONS(LOAD_FN)
#undef LOAD_FN
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 prop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};prop.pNext=&id;fnGetPhysicalDeviceProperties2(vk->phys_dev,&prop);
        if(!id.deviceLUIDValid)throw std::runtime_error("Vulkan LUID unavailable");
        ComPtr<IDXGIFactory1> factory;hrcheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT n=0;;n++){ComPtr<IDXGIAdapter1> candidate;HRESULT r=factory->EnumAdapters1(n,&candidate);if(r==DXGI_ERROR_NOT_FOUND)break;hrcheck(r);DXGI_ADAPTER_DESC1 d{};hrcheck(candidate->GetDesc1(&d));if(!std::memcmp(&d.AdapterLuid,id.deviceLUID,VK_LUID_SIZE)){adapter=candidate;break;}}
        if(!adapter)throw std::runtime_error("matching D3D11 adapter not found");
        D3D_FEATURE_LEVEL level;hrcheck(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx));
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};hrcheck(dev->CreateQuery(&query,&done));
        if(layers)compositor=std::make_unique<Compositor>(dev.Get(),ctx.Get(),layers);
        bool found=false;
        for(int i=0;i<vk->nb_qf;i++)if(vk->qf[i].flags&(VK_QUEUE_TRANSFER_BIT|VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)){family=vk->qf[i].idx;found=true;break;}
        if(!found)throw std::runtime_error("transfer queue unavailable");
        fnGetDeviceQueue(vk->act_dev,family,0,&queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pi.queueFamilyIndex=family;vkcheck(fnCreateCommandPool(vk->act_dev,&pi,nullptr,&pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;vkcheck(fnAllocateCommandBuffers(vk->act_dev,&ai,&cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkcheck(fnCreateFence(vk->act_dev,&fi,nullptr,&fence));
        std::cerr<<"interop_adapter="<<prop.properties.deviceName<<" luid_match=true feature_level="<<std::hex<<level<<std::dec<<"\n";
    }
    ~VulkanD3D11(){if(fnDeviceWaitIdle)fnDeviceWaitIdle(vk->act_dev);for(auto&p:planes){if(p.image)fnDestroyImage(vk->act_dev,p.image,nullptr);if(p.mem)fnFreeMemory(vk->act_dev,p.mem,nullptr);}if(fence)fnDestroyFence(vk->act_dev,fence,nullptr);if(pool)fnDestroyCommandPool(vk->act_dev,pool,nullptr);}
    void init(AVFrame* frame) {
        auto* fc=reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);auto* vfc=static_cast<AVVulkanFramesContext*>(fc->hwctx);
        const auto* desc=av_pix_fmt_desc_get(fc->sw_format);int count=av_pix_fmt_count_planes(fc->sw_format);
        if(width){if(width!=frame->width||height!=frame->height||format!=fc->sw_format)throw std::runtime_error("interop dynamic format change unsupported");return;}
        width=frame->width;height=frame->height;format=fc->sw_format;
        if(!desc||count<3||count>4)throw std::runtime_error("unsupported planar format");
        for(int i=0;i<count;i++) {
            if(vfc->format[i]!=VK_FORMAT_R16_UNORM)throw std::runtime_error("interop requires separate R16_UNORM images");
            VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};ext.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
            VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};info.pNext=&ext;info.format=VK_FORMAT_R16_UNORM;info.type=VK_IMAGE_TYPE_2D;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkExternalImageFormatProperties out{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};props.pNext=&out;
            vkcheck(fnGetPhysicalDeviceImageFormatProperties2(vk->phys_dev,&info,&props));
            if(!(out.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))throw std::runtime_error("R16 external image not importable");
            planes.emplace_back();auto& p=planes.back();int shift=i==1||i==2?desc->log2_chroma_w:0;int sh=i==1||i==2?desc->log2_chroma_h:0;p.w=(width+(1<<shift)-1)>>shift;p.h=(height+(1<<sh)-1)>>sh;
            D3D11_TEXTURE2D_DESC d{};d.Width=p.w;d.Height=p.h;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R16_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            hrcheck(dev->CreateTexture2D(&d,nullptr,&p.shared));hrcheck(p.shared.As(&p.mutex));d.MiscFlags=0;hrcheck(dev->CreateTexture2D(&d,nullptr,&p.consumer));
            ComPtr<IDXGIResource1> resource;hrcheck(p.shared.As(&resource));HANDLE handle=nullptr;hrcheck(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle));
            struct HandleCloser {HANDLE h;~HandleCloser(){CloseHandle(h);}} closer{handle};
            VkExternalMemoryImageCreateInfo ei{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};ei.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.pNext=&ei;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R16_UNORM;ci.extent={p.w,p.h,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            vkcheck(fnCreateImage(vk->act_dev,&ci,nullptr,&p.image));
            VkMemoryRequirements req;fnGetImageMemoryRequirements(vk->act_dev,p.image,&req);
            VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};vkcheck(fnGetMemoryWin32HandlePropertiesKHR(vk->act_dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,handle,&hp));
            uint32_t bits=req.memoryTypeBits&hp.memoryTypeBits,index=0;if(!bits)throw std::runtime_error("external memory types incompatible");while(!(bits&(1u<<index)))index++;
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=p.image;
            VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};imported.pNext=&dedicated;imported.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;imported.handle=handle;
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.pNext=&imported;alloc.allocationSize=req.size;alloc.memoryTypeIndex=index;
            vkcheck(fnAllocateMemory(vk->act_dev,&alloc,nullptr,&p.mem));vkcheck(fnBindImageMemory(vk->act_dev,p.image,p.mem,0));
        }
        std::cerr<<"interop_planes="<<planes.size()<<" sw_format="<<av_get_pix_fmt_name(fc->sw_format)<<" gpu_copies_per_plane=2\n";
    }
    void copy(AVFrame* frame) {
        init(frame);auto* fc=reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);auto* vfc=static_cast<AVVulkanFramesContext*>(fc->hwctx);auto* f=reinterpret_cast<AVVkFrame*>(frame->data[0]);
        vfc->lock_frame(fc,f);
        struct Unlock {AVHWFramesContext* fc;AVVulkanFramesContext* vfc;AVVkFrame* f;~Unlock(){vfc->unlock_frame(fc,f);}} unlock{fc,vfc,f};
        vkcheck(fnResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vkcheck(fnBeginCommandBuffer(cmd,&bi));
        std::vector<VkSemaphore> sem;std::vector<uint64_t> values,next,acquire,release;std::vector<VkPipelineStageFlags> stages;std::vector<VkDeviceMemory> memories;std::vector<uint32_t> timeouts;
        for(size_t i=0;i<planes.size();i++) {
            auto& p=planes[i];if(f->queue_family[i]!=VK_QUEUE_FAMILY_IGNORED&&f->queue_family[i]!=family)throw std::runtime_error("FFmpeg queue ownership unsupported");
            VkImageMemoryBarrier source{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};source.srcAccessMask=f->access[i];source.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;source.oldLayout=f->layout[i];source.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;source.srcQueueFamilyIndex=source.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;source.image=f->img[i];source.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            VkImageMemoryBarrier dest{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};dest.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;dest.oldLayout=p.initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;dest.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;dest.srcQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;dest.dstQueueFamilyIndex=family;dest.image=p.image;dest.subresourceRange=source.subresourceRange;
            VkImageMemoryBarrier barriers[]={source,dest};fnCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers);
            VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;region.extent={p.w,p.h,1};fnCmdCopyImage(cmd,f->img[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,p.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            std::swap(source.oldLayout,source.newLayout);source.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;source.dstAccessMask=f->access[i];dest.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;dest.newLayout=VK_IMAGE_LAYOUT_GENERAL;dest.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;dest.dstAccessMask=0;dest.srcQueueFamilyIndex=family;dest.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
            VkImageMemoryBarrier after[]={source,dest};fnCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,2,after);
            sem.push_back(f->sem[i]);values.push_back(f->sem_value[i]);next.push_back(f->sem_value[i]+1);stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);memories.push_back(p.mem);acquire.push_back(0);release.push_back(1);timeouts.push_back(10000);
        }
        vkcheck(fnEndCommandBuffer(cmd));
        VkWin32KeyedMutexAcquireReleaseInfoKHR keyed{VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};keyed.acquireCount=keyed.releaseCount=static_cast<uint32_t>(planes.size());keyed.pAcquireSyncs=keyed.pReleaseSyncs=memories.data();keyed.pAcquireKeys=acquire.data();keyed.pReleaseKeys=release.data();keyed.pAcquireTimeouts=timeouts.data();
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};timeline.pNext=&keyed;timeline.waitSemaphoreValueCount=timeline.signalSemaphoreValueCount=static_cast<uint32_t>(sem.size());timeline.pWaitSemaphoreValues=values.data();timeline.pSignalSemaphoreValues=next.data();
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.pNext=&timeline;submit.waitSemaphoreCount=submit.signalSemaphoreCount=static_cast<uint32_t>(sem.size());submit.pWaitSemaphores=submit.pSignalSemaphores=sem.data();submit.pWaitDstStageMask=stages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
        vkcheck(fnResetFences(vk->act_dev,1,&fence));hw_queue_lock();auto result=fnQueueSubmit(queue,1,&submit,fence);hw_queue_unlock();vkcheck(result);
        for(size_t i=0;i<planes.size();i++){f->sem_value[i]=next[i];planes[i].initialized=true;}
        vkcheck(fnWaitForFences(vk->act_dev,1,&fence,VK_TRUE,10000000000ULL));
        for(auto& p:planes){auto r=p.mutex->AcquireSync(1,10000);if(r!=S_OK)throw std::runtime_error("D3D mutex acquire failed");ctx->CopyResource(p.consumer.Get(),p.shared.Get());}
        if(compositor){std::vector<ComPtr<ID3D11Texture2D>> textures;for(auto& p:planes)textures.push_back(p.consumer);compositor->render(textures,static_cast<AVPixelFormat>(format),width,height);}
        ctx->End(done.Get());ctx->Flush();auto start=Clock::now();
        for(;;){auto r=ctx->GetData(done.Get(),nullptr,0,0);hrcheck(r);if(r==S_OK)break;if(ms(start,Clock::now())>10000)throw std::runtime_error("consumer timeout");SwitchToThread();}
        for(auto& p:planes)hrcheck(p.mutex->ReleaseSync(0));
    }
    void hw_queue_lock(){if(vk->lock_queue)vk->lock_queue(hw,family,0);}
    void hw_queue_unlock(){if(vk->unlock_queue)vk->unlock_queue(hw,family,0);}
    void verify(AVFrame* frame) {
        if(checked)return;
        AVFrame* downloaded=av_frame_alloc();if(!downloaded)throw std::bad_alloc();
        struct FreeFrame{AVFrame* f;~FreeFrame(){av_frame_free(&f);}} free{downloaded};
        avcheck(av_hwframe_transfer_data(downloaded,frame,0));
        for(size_t i=0;i<planes.size();i++) {
            auto& p=planes[i];D3D11_TEXTURE2D_DESC d;p.consumer->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;hrcheck(dev->CreateTexture2D(&d,nullptr,&staging));ctx->CopyResource(staging.Get(),p.consumer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped;hrcheck(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));bool same=true;
            for(uint32_t y=0;y<p.h;y++)if(std::memcmp(static_cast<char*>(mapped.pData)+y*mapped.RowPitch,downloaded->data[i]+y*downloaded->linesize[i],p.w*2)){same=false;break;}
            ctx->Unmap(staging.Get(),0);if(!same)throw std::runtime_error("interop pixels differ from Vulkan download");
        }
        if(compositor)compositor->verify(downloaded);
        checked=true;std::cerr<<"interop_first_frame_all_planes_exact=true (readback QA only)\n";
    }
};
#undef VK_FUNCTIONS
