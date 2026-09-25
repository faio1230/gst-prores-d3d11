// Windows向けの実験用GstVideoDecoder。復号はVulkan、出力はsystem memory。
#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/video/video.h>
#include <climits>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/intreadwrite.h>
}

GST_DEBUG_CATEGORY_STATIC(proresvk_debug);
#define GST_CAT_DEFAULT proresvk_debug

typedef struct _GstProresVkDec {
    GstVideoDecoder parent;
    AVCodecContext* codec;
    AVBufferRef* device;
    AVPacket* packet;
    AVFrame* decoded;
    AVFrame* downloaded;
    GstVideoCodecState* input;
    GstVideoInfo output_info;
    gboolean negotiated;
    gboolean failed;
    gboolean drained;
    gint flushing;
    guint device_index;
    guint wait_timeout;
    gint color_range, color_space, color_primaries, color_trc;
} GstProresVkDec;

typedef struct _GstProresVkDecClass { GstVideoDecoderClass parent_class; } GstProresVkDecClass;
G_DEFINE_TYPE(GstProresVkDec, gst_prores_vk_dec, GST_TYPE_VIDEO_DECODER)
#define SELF(obj) (reinterpret_cast<GstProresVkDec*>(obj))

enum { PROP_0, PROP_DEVICE_INDEX, PROP_WAIT_TIMEOUT };
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-prores, variant=(string){proxy,lt,standard,hq}, "
                    "width=(int)[16,8192], height=(int)[16,8192], interlace-mode=(string)progressive"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format=(string)I422_10LE, "
                    "width=(int)[16,8192], height=(int)[16,8192], interlace-mode=(string)progressive"));

static void clear_codec(GstProresVkDec* self) {
    av_packet_free(&self->packet);
    av_frame_free(&self->downloaded);
    av_frame_free(&self->decoded);
    avcodec_free_context(&self->codec);
    if (self->input) { gst_video_codec_state_unref(self->input); self->input = nullptr; }
    self->negotiated = FALSE;
    self->drained = FALSE;
}

static void post_av_error(GstProresVkDec* self, const char* operation, int code) {
    char error[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, error, sizeof(error));
    self->failed = TRUE;
    GST_ELEMENT_ERROR(self, STREAM, DECODE, ("ProRes Vulkan decode failed: %s", operation),
                      ("FFmpeg: %s (%d); CPU fallback is disabled", error, code));
}

static AVPixelFormat select_vulkan(AVCodecContext*, const AVPixelFormat* formats) {
    for (; *formats != AV_PIX_FMT_NONE; ++formats)
        if (*formats == AV_PIX_FMT_VULKAN) return *formats;
    return AV_PIX_FMT_NONE;
}

static gboolean start(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    self->failed = FALSE;
    g_atomic_int_set(&self->flushing, 0);
    return TRUE;
}

static gboolean stop(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    clear_codec(self);
    av_buffer_unref(&self->device);
    return TRUE;
}

static gboolean set_format(GstVideoDecoder* decoder, GstVideoCodecState* state) {
    auto* self = SELF(decoder);
    clear_codec(self);
    self->failed = FALSE;
    const auto* s = gst_caps_get_structure(state->caps, 0);
    const char* variant = gst_structure_get_string(s, "variant");
    const char* interlace = gst_structure_get_string(s, "interlace-mode");
    const int width = GST_VIDEO_INFO_WIDTH(&state->info);
    const int height = GST_VIDEO_INFO_HEIGHT(&state->info);
    guint32 tag = 0;
    if (g_strcmp0(variant, "proxy") == 0) tag = MKTAG('a','p','c','o');
    else if (g_strcmp0(variant, "lt") == 0) tag = MKTAG('a','p','c','s');
    else if (g_strcmp0(variant, "standard") == 0) tag = MKTAG('a','p','c','n');
    else if (g_strcmp0(variant, "hq") == 0) tag = MKTAG('a','p','c','h');
    if (!tag || (interlace && g_strcmp0(interlace, "progressive") != 0) ||
        width < 16 || width > 8192 || height < 16 || height > 8192 || (width & 1)) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT,
            ("Only progressive ProRes 422 with even width is supported"), ("caps: %" GST_PTR_FORMAT, state->caps));
        return FALSE;
    }
    if (!self->device) {
        gchar* index = g_strdup_printf("%u", self->device_index);
        const int ret = av_hwdevice_ctx_create(&self->device, AV_HWDEVICE_TYPE_VULKAN, index, nullptr, 0);
        g_free(index);
        if (ret < 0) { post_av_error(self, "create Vulkan device", ret); return FALSE; }
        auto* hw = reinterpret_cast<AVHWDeviceContext*>(self->device->data);
        auto* vk = static_cast<AVVulkanDeviceContext*>(hw->hwctx);
        auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(vk->get_proc_addr(vk->inst, "vkGetPhysicalDeviceProperties"));
        VkPhysicalDeviceProperties properties{};
        if (get_properties) get_properties(vk->phys_dev, &properties);
        if (!get_properties || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            av_buffer_unref(&self->device);
            post_av_error(self, "physical GPU required (software Vulkan rejected)", AVERROR(ENOSYS));
            return FALSE;
        }
        GST_INFO_OBJECT(self, "Vulkan adapter: %s", properties.deviceName);
    }
    const auto* implementation = avcodec_find_decoder_by_name("prores");
    if (!implementation) { post_av_error(self, "find prores", AVERROR_DECODER_NOT_FOUND); return FALSE; }
    self->codec = avcodec_alloc_context3(implementation);
    self->packet = av_packet_alloc();
    self->decoded = av_frame_alloc();
    self->downloaded = av_frame_alloc();
    if (!self->codec || !self->packet || !self->decoded || !self->downloaded) {
        post_av_error(self, "allocate decoder", AVERROR(ENOMEM)); return FALSE;
    }
    self->codec->width = width;
    self->codec->height = height;
    self->codec->codec_tag = tag;
    self->codec->bits_per_raw_sample = 10;
    // ProResのフレーム単位入出力を同期的に処理し、フレームスレッドの遅延を避ける。
    self->codec->thread_count = 1;
    self->codec->thread_type = FF_THREAD_SLICE;
    self->codec->get_format = select_vulkan;
    self->codec->hw_device_ctx = av_buffer_ref(self->device);
    if (!self->codec->hw_device_ctx) { post_av_error(self, "reference Vulkan device", AVERROR(ENOMEM)); return FALSE; }
    self->codec->pkt_timebase = AVRational{1, 1000000000};
    self->codec->err_recognition = AV_EF_EXPLODE;
    const int ret = avcodec_open2(self->codec, implementation, nullptr);
    if (ret < 0) { post_av_error(self, "open prores", ret); return FALSE; }
    self->input = gst_video_codec_state_ref(state);
    GST_INFO_OBJECT(self, "Vulkan device %u; progressive 422 %dx%d; output=system-memory I422_10LE", self->device_index, width, height);
    return TRUE;
}

// コピーしたsemaphore値を待つ間もAVFrameを保持する。FLUSH_STARTを短い間隔で確認する。
static GstFlowReturn wait_for_gpu(GstProresVkDec* self) {
    auto* fc = reinterpret_cast<AVHWFramesContext*>(self->decoded->hw_frames_ctx->data);
    auto* vfc = static_cast<AVVulkanFramesContext*>(fc->hwctx);
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(self->device->data);
    auto* vk = static_cast<AVVulkanDeviceContext*>(hw->hwctx);
    auto* frame = reinterpret_cast<AVVkFrame*>(self->decoded->data[0]);
    auto wait = reinterpret_cast<PFN_vkWaitSemaphores>(vk->get_proc_addr(vk->inst, "vkWaitSemaphores"));
    if (!wait) { post_av_error(self, "load vkWaitSemaphores", AVERROR_EXTERNAL); return GST_FLOW_ERROR; }
    VkSemaphore semaphores[AV_NUM_DATA_POINTERS]{};
    uint64_t values[AV_NUM_DATA_POINTERS]{};
    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.pSemaphores = semaphores;
    info.pValues = values;
    vfc->lock_frame(fc, frame);
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
        if (!frame->sem[i]) continue;
        semaphores[info.semaphoreCount] = frame->sem[i];
        values[info.semaphoreCount++] = frame->sem_value[i];
    }
    vfc->unlock_frame(fc, frame);
    if (!info.semaphoreCount) { post_av_error(self, "missing Vulkan synchronization", AVERROR_INVALIDDATA); return GST_FLOW_ERROR; }
    const gint64 deadline = g_get_monotonic_time() + static_cast<gint64>(self->wait_timeout) * 1000;
    for (;;) {
        if (g_atomic_int_get(&self->flushing)) return GST_FLOW_FLUSHING;
        const VkResult result = wait(vk->act_dev, &info, 10000000ULL);
        if (result == VK_SUCCESS) return GST_FLOW_OK;
        if (result != VK_TIMEOUT || g_get_monotonic_time() >= deadline) {
            self->failed = TRUE;
            GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("Vulkan frame wait failed"), ("VkResult=%d", result));
            return GST_FLOW_ERROR;
        }
    }
}

static gboolean negotiate_output(GstProresVkDec* self) {
    const auto* f = self->decoded;
    if (self->negotiated && self->color_range == f->color_range && self->color_space == f->colorspace &&
        self->color_primaries == f->color_primaries && self->color_trc == f->color_trc) return TRUE;
    auto* decoder = GST_VIDEO_DECODER(self);
    auto* state = gst_video_decoder_set_output_state(decoder, GST_VIDEO_FORMAT_I422_10LE, f->width, f->height, self->input);
    if (!state) return FALSE;
    state->info.interlace_mode = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
    auto& color = state->info.colorimetry;
    color.range = f->color_range == AVCOL_RANGE_MPEG ? GST_VIDEO_COLOR_RANGE_16_235 :
                  f->color_range == AVCOL_RANGE_JPEG ? GST_VIDEO_COLOR_RANGE_0_255 : GST_VIDEO_COLOR_RANGE_UNKNOWN;
    color.matrix = gst_video_color_matrix_from_iso(f->colorspace);
    color.primaries = gst_video_color_primaries_from_iso(f->color_primaries);
    color.transfer = gst_video_transfer_function_from_iso(f->color_trc);
    // コンテナ由来の色情報がcapsに明示されている場合だけ未指定項目を補う。
    const auto* input_caps = gst_caps_get_structure(self->input->caps, 0);
    GstVideoColorimetry upstream{};
    const char* description = gst_structure_get_string(input_caps, "colorimetry");
    if (description && gst_video_colorimetry_from_string(&upstream, description)) {
        if (color.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN) color.matrix = upstream.matrix;
        if (color.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN) color.primaries = upstream.primaries;
        if (color.transfer == GST_VIDEO_TRANSFER_UNKNOWN) color.transfer = upstream.transfer;
    }
    self->output_info = state->info;
    gst_video_codec_state_unref(state);
    if (!gst_video_decoder_negotiate(decoder)) return FALSE;
    self->color_range = f->color_range; self->color_space = f->colorspace;
    self->color_primaries = f->color_primaries; self->color_trc = f->color_trc;
    self->negotiated = TRUE;
    return TRUE;
}

static GstFlowReturn decode_frame(GstProresVkDec* self, GstVideoCodecFrame* frame) {
    auto* decoder = GST_VIDEO_DECODER(self);
    if (self->failed || !self->codec) return GST_FLOW_ERROR;
    if (g_atomic_int_get(&self->flushing)) return GST_FLOW_FLUSHING;
    const gsize size = gst_buffer_get_size(frame->input_buffer);
    guint8 header[28]{};
    if (size < sizeof(header) || size > 128 * 1024 * 1024 ||
        gst_buffer_extract(frame->input_buffer, 0, header, sizeof(header)) != sizeof(header) ||
        std::memcmp(header + 4, "icpf", 4) != 0 || AV_RB32(header) != size ||
        AV_RB16(header + 8) < 20 || AV_RB16(header + 8) > size - 8 ||
        AV_RB16(header + 10) > 1 || (header[20] & 0xc0) != 0x80 ||
        ((header[20] >> 2) & 3) != 0 || (header[25] & 15) != 0 ||
        AV_RB16(header + 16) != self->codec->width || AV_RB16(header + 18) != self->codec->height) {
        self->failed = TRUE;
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Unsupported or malformed ProRes frame"),
            ("Require one complete progressive 422 frame, no alpha, dimensions matching caps; bytes=%" G_GSIZE_FORMAT, size));
        return GST_FLOW_ERROR;
    }
    if (self->drained) { avcodec_flush_buffers(self->codec); self->drained = FALSE; }
    // FFmpegのframe初期化より前に当該ProResヘッダーの色識別子を設定する。
    self->codec->color_primaries = static_cast<AVColorPrimaries>(header[22]);
    self->codec->color_trc = static_cast<AVColorTransferCharacteristic>(header[23]);
    self->codec->colorspace = static_cast<AVColorSpace>(header[24]);
    self->codec->color_range = AVCOL_RANGE_MPEG;
    av_packet_unref(self->packet);
    int ret = av_new_packet(self->packet, static_cast<int>(size));
    if (ret < 0) { post_av_error(self, "allocate packet", ret); return GST_FLOW_ERROR; }
    // av_new_packetが末尾paddingを確保する。入力GstBufferの寿命に依存しない。
    if (gst_buffer_extract(frame->input_buffer, 0, self->packet->data, size) != size) {
        post_av_error(self, "map input packet", AVERROR(EIO)); return GST_FLOW_ERROR;
    }
    self->packet->pts = GST_CLOCK_TIME_IS_VALID(frame->pts) && frame->pts <= G_MAXINT64 ? static_cast<int64_t>(frame->pts) : AV_NOPTS_VALUE;
    self->packet->dts = GST_CLOCK_TIME_IS_VALID(frame->dts) && frame->dts <= G_MAXINT64 ? static_cast<int64_t>(frame->dts) : AV_NOPTS_VALUE;
    if (self->decoded) av_frame_unref(self->decoded);
    if (self->downloaded) av_frame_unref(self->downloaded);
    ret = avcodec_send_packet(self->codec, self->packet);
    av_packet_unref(self->packet);
    if (ret < 0) { post_av_error(self, "send packet", ret); return GST_FLOW_ERROR; }
    ret = avcodec_receive_frame(self->codec, self->decoded);
    if (ret < 0) { post_av_error(self, "receive frame (synchronous ProRes required)", ret); return GST_FLOW_ERROR; }
    if (self->decoded->format != AV_PIX_FMT_VULKAN || !self->decoded->hw_frames_ctx ||
        (self->decoded->flags & (AV_FRAME_FLAG_CORRUPT | AV_FRAME_FLAG_INTERLACED)) || self->decoded->decode_error_flags) {
        post_av_error(self, "unexpected decoded frame or software fallback", AVERROR_INVALIDDATA); return GST_FLOW_ERROR;
    }
    const auto* frames = reinterpret_cast<AVHWFramesContext*>(self->decoded->hw_frames_ctx->data);
    if (frames->sw_format != AV_PIX_FMT_YUV422P10LE || self->decoded->width != self->codec->width || self->decoded->height != self->codec->height) {
        post_av_error(self, "unexpected output format", AVERROR_INVALIDDATA); return GST_FLOW_ERROR;
    }
    auto flow = wait_for_gpu(self);
    if (flow != GST_FLOW_OK) return flow;
    ret = av_hwframe_transfer_data(self->downloaded, self->decoded, 0);
    if (ret < 0) { post_av_error(self, "download frame", ret); return GST_FLOW_ERROR; }
    if (!negotiate_output(self)) return GST_FLOW_NOT_NEGOTIATED;
    flow = gst_video_decoder_allocate_output_frame(decoder, frame);
    if (flow != GST_FLOW_OK) return flow;
    GstVideoFrame output{};
    if (!gst_video_frame_map(&output, &self->output_info, frame->output_buffer, GST_MAP_WRITE)) {
        post_av_error(self, "map output buffer", AVERROR(EIO)); return GST_FLOW_ERROR;
    }
    for (guint plane = 0; plane < 3; ++plane) {
        const int bytes = (plane ? self->decoded->width / 2 : self->decoded->width) * 2;
        for (int y = 0; y < self->decoded->height; ++y)
            std::memcpy(static_cast<guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&output, plane)) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&output, plane),
                        self->downloaded->data[plane] + y * self->downloaded->linesize[plane], bytes);
    }
    gst_video_frame_unmap(&output);
    GST_LOG_OBJECT(self, "Vulkan decoded frame %u pts=%" GST_TIME_FORMAT, frame->system_frame_number, GST_TIME_ARGS(frame->pts));
    return GST_FLOW_OK;
}

static GstFlowReturn handle_frame(GstVideoDecoder* decoder, GstVideoCodecFrame* frame) {
    auto* self = SELF(decoder);
    const auto flow = decode_frame(self, frame);
    // 出力は独立したGStreamer所有バッファ。下流はAVFrame/deviceを保持しなくてよい。
    if (self->decoded) av_frame_unref(self->decoded);
    if (self->downloaded) av_frame_unref(self->downloaded);
    if (flow != GST_FLOW_OK) { gst_video_decoder_drop_frame(decoder, frame); return flow; }
    GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT(frame);
    return gst_video_decoder_finish_frame(decoder, frame);
}

static gboolean flush(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    if (self->codec) avcodec_flush_buffers(self->codec);
    if (self->decoded) av_frame_unref(self->decoded);
    if (self->downloaded) av_frame_unref(self->downloaded);
    if (self->packet) av_packet_unref(self->packet);
    self->drained = FALSE;
    return TRUE;
}

static GstFlowReturn finish(GstVideoDecoder* decoder) {
    auto* self = SELF(decoder);
    if (!self->codec || self->drained) return GST_FLOW_OK;
    if (self->failed) return GST_FLOW_ERROR;
    if (g_atomic_int_get(&self->flushing)) return GST_FLOW_FLUSHING;
    int ret = avcodec_send_packet(self->codec, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) { post_av_error(self, "drain send", ret); return GST_FLOW_ERROR; }
    ret = avcodec_receive_frame(self->codec, self->decoded);
    if (ret != AVERROR_EOF) { post_av_error(self, "unexpected delayed output at EOS", ret < 0 ? ret : AVERROR_INVALIDDATA); return GST_FLOW_ERROR; }
    self->drained = TRUE;
    return GST_FLOW_OK;
}

static GstFlowReturn drain(GstVideoDecoder* decoder) {
    const auto flow = finish(decoder);
    if (flow == GST_FLOW_OK) flush(decoder);
    return flow;
}

static gboolean sink_event(GstVideoDecoder* decoder, GstEvent* event) {
    auto* self = SELF(decoder);
    const auto type = GST_EVENT_TYPE(event);
    if (type == GST_EVENT_FLUSH_START) g_atomic_int_set(&self->flushing, 1);
    const auto result = GST_VIDEO_DECODER_CLASS(gst_prores_vk_dec_parent_class)->sink_event(decoder, event);
    if (type == GST_EVENT_FLUSH_STOP) g_atomic_int_set(&self->flushing, 0);
    return result;
}

static void set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec) {
    auto* self = SELF(object);
    GST_OBJECT_LOCK(self);
    if (GST_STATE(self) > GST_STATE_READY) {
        GST_OBJECT_UNLOCK(self);
        GST_WARNING_OBJECT(self, "Properties can only be changed in NULL/READY");
        return;
    }
    switch (id) {
        case PROP_DEVICE_INDEX: self->device_index = g_value_get_uint(value); break;
        case PROP_WAIT_TIMEOUT: self->wait_timeout = g_value_get_uint(value); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
    }
    GST_OBJECT_UNLOCK(self);
}

static void get_property(GObject* object, guint id, GValue* value, GParamSpec* spec) {
    auto* self = SELF(object);
    GST_OBJECT_LOCK(self);
    switch (id) {
        case PROP_DEVICE_INDEX: g_value_set_uint(value, self->device_index); break;
        case PROP_WAIT_TIMEOUT: g_value_set_uint(value, self->wait_timeout); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec); break;
    }
    GST_OBJECT_UNLOCK(self);
}

static void finalize(GObject* object) {
    auto* self = SELF(object);
    clear_codec(self);
    av_buffer_unref(&self->device);
    G_OBJECT_CLASS(gst_prores_vk_dec_parent_class)->finalize(object);
}

static void gst_prores_vk_dec_class_init(GstProresVkDecClass* klass) {
    auto* object = G_OBJECT_CLASS(klass);
    object->set_property = set_property; object->get_property = get_property; object->finalize = finalize;
    const auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(object, PROP_DEVICE_INDEX,
        g_param_spec_uint("device-index", "Vulkan device", "Vulkan physical device index (CPU fallback disabled)", 0, 31, 0, flags));
    g_object_class_install_property(object, PROP_WAIT_TIMEOUT,
        g_param_spec_uint("gpu-wait-timeout-ms", "GPU wait timeout", "Timeout for frame semaphore wait; does not bound FFmpeg internal calls", 10, 60000, 10000, flags));
    auto* element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Experimental ProRes Vulkan decoder", "Codec/Decoder/Video/Hardware",
        "GPU decode of progressive ProRes 422; 10-bit system-memory output", "ProRes GPU project");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
    auto* decoder = GST_VIDEO_DECODER_CLASS(klass);
    decoder->start = start; decoder->stop = stop; decoder->set_format = set_format;
    decoder->handle_frame = handle_frame; decoder->flush = flush;
    decoder->finish = finish; decoder->drain = drain; decoder->sink_event = sink_event;
}

static void gst_prores_vk_dec_init(GstProresVkDec* self) {
    self->wait_timeout = 10000;
    gst_video_info_init(&self->output_info);
    gst_video_decoder_set_packetized(GST_VIDEO_DECODER(self), TRUE);
    gst_video_decoder_set_needs_format(GST_VIDEO_DECODER(self), TRUE);
}

static gboolean plugin_init(GstPlugin* plugin) {
    GST_DEBUG_CATEGORY_INIT(proresvk_debug, "proresvkdec", 0, "ProRes Vulkan decoder");
    return gst_element_register(plugin, "proresvkdec", GST_RANK_NONE, gst_prores_vk_dec_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, proresvk,
    "Experimental standalone ProRes GPU decoder", plugin_init, "0.1.0", "unknown",
    "prores-gpu-lab", "https://example.invalid/prores-gpu-lab")
