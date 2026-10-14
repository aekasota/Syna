#include "video/video_decoder.h"

#include "common/log.h"

#include <algorithm>
#include <thread>

namespace syna {
namespace {

DXGI_FORMAT dxgi_for(AVPixelFormat sw) {
    switch (sw) {
        case AV_PIX_FMT_NV12: return DXGI_FORMAT_NV12;
        case AV_PIX_FMT_P010: return DXGI_FORMAT_P010;
        case AV_PIX_FMT_P016: return DXGI_FORMAT_P016;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool can_sample(ID3D11Device* dev, DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_UNKNOWN) return false;
    UINT support = 0;
    if (FAILED(dev->CheckFormatSupport(f, &support))) return false;
    return (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D);
}

bool codec_has_d3d11va(const AVCodec* codec) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) return false;
        if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return true;
    }
}

}  // namespace

bool VideoDecoder::create_hw_device(ID3D11Device* device) {
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return false;
    auto* hw = reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(ref->data)->hwctx);
    hw->device = device;
    device->AddRef();  // released by FFmpeg with the device context
    int r = av_hwdevice_ctx_init(ref);
    if (r < 0) {
        av_buffer_unref(&ref);
        hw_failure_ = "D3D11VA device init failed: " + av_error_string(r);
        return false;
    }
    hw_device_.reset(ref);
    return true;
}

bool VideoDecoder::open(AVStream* stream, ID3D11Device* device, bool prefer_hw, int extra_surfaces, int sw_threads,
                        std::string* error) {
    device_ = device;
    const AVCodecParameters* par = stream->codecpar;

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    bool want_hw = prefer_hw && device != nullptr;
    if (want_hw && par->codec_id == AV_CODEC_ID_AV1) {
        // libdav1d is preferred for software AV1 but has no hwaccel; FFmpeg's native decoder does.
        if (const AVCodec* native = avcodec_find_decoder_by_name("av1")) codec = native;
    }
    if (!codec) {
        if (error) *error = std::string("no decoder for ") + avcodec_get_name(par->codec_id);
        return false;
    }
    if (want_hw && !codec_has_d3d11va(codec)) {
        hw_failure_ = std::string("no D3D11VA support for ") + codec->name;
        want_hw = false;
        if (par->codec_id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder(par->codec_id);
    }
    if (want_hw && !create_hw_device(device)) want_hw = false;
    if (!prefer_hw) hw_failure_ = "hardware decoding disabled in settings";
    else if (!device) hw_failure_ = "no D3D11 device";

    for (int attempt = 0; attempt < 2; ++attempt) {
        ctx_.reset(avcodec_alloc_context3(codec));
        if (!ctx_) {
            if (error) *error = "out of memory";
            return false;
        }
        int r = avcodec_parameters_to_context(ctx_.get(), par);
        if (r < 0) {
            if (error) *error = "bad codec parameters: " + av_error_string(r);
            return false;
        }
        ctx_->pkt_timebase = stream->time_base;
        ctx_->opaque = this;
        if (hw_device_) {
            ctx_->get_format = &VideoDecoder::get_format;
            ctx_->hw_device_ctx = av_buffer_ref(hw_device_.get());
            ctx_->extra_hw_frames = extra_surfaces;
            ctx_->thread_count = 1;
        } else {
            ctx_->thread_count = sw_threads > 0 ? sw_threads : std::min(16, static_cast<int>(std::thread::hardware_concurrency()));
            ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        r = avcodec_open2(ctx_.get(), codec, nullptr);
        if (r >= 0) break;
        if (hw_device_) {
            log::warn("decoder", "opening {} with D3D11VA failed ({}); falling back to software", codec->name, av_error_string(r));
            hw_failure_ = "decoder open failed with hardware: " + av_error_string(r);
            hw_device_.reset();
            if (par->codec_id == AV_CODEC_ID_AV1) codec = avcodec_find_decoder(par->codec_id);
            continue;
        }
        if (error) *error = std::string("cannot open decoder ") + codec->name + ": " + av_error_string(r);
        return false;
    }
    log::info("decoder", "opened codec={} {}x{} hw_requested={} threads={}", ctx_->codec->name, par->width, par->height,
              hw_device_ != nullptr, ctx_->thread_count);
    return true;
}

AVPixelFormat VideoDecoder::get_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
    auto* self = static_cast<VideoDecoder*>(ctx->opaque);
    if (self->hw_device_) {
        bool offered = false;
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == AV_PIX_FMT_D3D11) {
                offered = true;
                if (self->setup_hw_frames(ctx)) return AV_PIX_FMT_D3D11;
                break;
            }
        }
        if (!offered && self->hw_failure_.empty())
            self->hw_failure_ = "the hardware decoder does not support this stream's profile or format";
    }
    av_buffer_unref(&ctx->hw_frames_ctx);
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

bool VideoDecoder::setup_hw_frames(AVCodecContext* ctx) {
    av_buffer_unref(&ctx->hw_frames_ctx);
    for (int pass = 0; pass < 2; ++pass) {
        AVBufferRef* ref = nullptr;
        int r = avcodec_get_hw_frames_parameters(ctx, hw_device_.get(), AV_PIX_FMT_D3D11, &ref);
        if (r < 0) {
            hw_failure_ = "hardware decoder rejected the stream: " + av_error_string(r);
            return false;
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(ref->data);
        auto* d3d = reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx);
        // initial_pool_size already includes ctx->extra_hw_frames (DPB + queue headroom).
        const bool sample = pass == 0 && can_sample(device_, dxgi_for(frames->sw_format));
        d3d->BindFlags = D3D11_BIND_DECODER | (sample ? D3D11_BIND_SHADER_RESOURCE : 0);
        r = av_hwframe_ctx_init(ref);
        if (r >= 0) {
            pool_size_ = frames->initial_pool_size;
            shader_bound_ = sample;
            ctx->hw_frames_ctx = ref;
            log::info("decoder", "d3d11va surfaces format={} pool={} shader_bound={}", av_get_pix_fmt_name(frames->sw_format),
                      pool_size_, sample);
            return true;
        }
        av_buffer_unref(&ref);
        if (!sample) {
            hw_failure_ = "cannot allocate decoder surfaces: " + av_error_string(r);
            return false;
        }
        log::warn("decoder", "shader-readable decoder surfaces rejected ({}), retrying decoder-only", av_error_string(r));
    }
    return false;
}

std::string VideoDecoder::codec_name() const { return ctx_ && ctx_->codec ? ctx_->codec->name : ""; }

}  // namespace syna
