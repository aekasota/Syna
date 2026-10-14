#include "trim/encoders.h"

#include "common/log.h"

#include <map>
#include <mutex>
#include <sstream>

namespace syna {
namespace {

const std::vector<EncoderInfo>& candidates(AVCodecID id) {
    static const std::vector<EncoderInfo> h264 = {
        {"h264_nvenc", "", "NVIDIA NVENC (H.264)", true},
        {"h264_amf", "", "AMD AMF (H.264)", true},
        {"h264_qsv", "", "Intel Quick Sync (H.264)", true},
        {"h264_mf", "hw_encoding=1", "GPU encoder via Media Foundation (H.264)", true},
        {"h264_mf", "", "Media Foundation software (H.264)", false},
        {"libopenh264", "", "OpenH264 (CPU)", false},
    };
    static const std::vector<EncoderInfo> hevc = {
        {"hevc_nvenc", "", "NVIDIA NVENC (HEVC)", true},
        {"hevc_amf", "", "AMD AMF (HEVC)", true},
        {"hevc_qsv", "", "Intel Quick Sync (HEVC)", true},
        {"hevc_mf", "hw_encoding=1", "GPU encoder via Media Foundation (HEVC)", true},
        {"libkvazaar", "", "Kvazaar (CPU)", false},
    };
    static const std::vector<EncoderInfo> av1 = {
        {"av1_nvenc", "", "NVIDIA NVENC (AV1)", true},
        {"av1_amf", "", "AMD AMF (AV1)", true},
        {"av1_qsv", "", "Intel Quick Sync (AV1)", true},
        {"libsvtav1", "", "SVT-AV1 (CPU)", false},
    };
    static const std::vector<EncoderInfo> vp9 = {
        {"vp9_qsv", "", "Intel Quick Sync (VP9)", true},
        {"libvpx-vp9", "", "libvpx VP9 (CPU)", false},
    };
    static const std::vector<EncoderInfo> none;
    switch (id) {
        case AV_CODEC_ID_H264: return h264;
        case AV_CODEC_ID_HEVC: return hevc;
        case AV_CODEC_ID_AV1: return av1;
        case AV_CODEC_ID_VP9: return vp9;
        default: return none;
    }
}

// Opens the encoder at a small 8-bit size to learn whether the driver/runtime supports it.
bool probe(const EncoderInfo& e, bool lossless) {
    static std::mutex m;
    static std::map<std::string, bool> cache;
    const std::string key = e.name + "|" + e.options + (lossless ? "|lossless" : "");
    std::lock_guard lock(m);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    bool ok = false;
    if (const AVCodec* codec = avcodec_find_encoder_by_name(e.name.c_str())) {
        CodecPtr ctx(avcodec_alloc_context3(codec));
        ctx->width = 640;
        ctx->height = 360;
        ctx->time_base = {1, 60};
        ctx->framerate = {60, 1};
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        const void* formats = nullptr;
        int n = 0;
        if (avcodec_get_supported_config(ctx.get(), codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats, &n) >= 0 && formats && n > 0) {
            const auto* list = static_cast<const AVPixelFormat*>(formats);
            ctx->pix_fmt = list[0];
            for (int i = 0; i < n; ++i) {
                if (list[i] == AV_PIX_FMT_NV12) {
                    ctx->pix_fmt = AV_PIX_FMT_NV12;
                    break;
                }
                if (list[i] == AV_PIX_FMT_YUV420P) ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            }
        }
        apply_encoder_options(ctx.get(), e);
        if (!lossless || configure_lossless(ctx.get(), e)) {
            const int r = avcodec_open2(ctx.get(), codec, nullptr);
            ok = r >= 0;
            log::info("encoders", "probe {}{}{}{} -> {}", e.name, e.options.empty() ? "" : " ", e.options, lossless ? " lossless" : "",
                      ok ? "available" : av_error_string(r));
        }
    }
    cache[key] = ok;
    return ok;
}

}  // namespace

bool encoder_available(const EncoderInfo& e) { return probe(e, false); }

bool encoder_supports_lossless(const EncoderInfo& e) { return encoder_available(e) && probe(e, true); }

bool configure_lossless(AVCodecContext* ctx, const EncoderInfo& e) {
    if (e.name.find("_nvenc") != std::string::npos) return av_opt_set(ctx->priv_data, "tune", "lossless", 0) >= 0;
    if (e.name == "libvpx-vp9") return av_opt_set_int(ctx->priv_data, "lossless", 1, 0) >= 0;
    return false;
}

void apply_encoder_options(AVCodecContext* ctx, const EncoderInfo& e) {
    std::stringstream ss(e.options);
    std::string kv;
    while (std::getline(ss, kv, ':')) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) av_opt_set(ctx->priv_data, kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 0);
    }
}

std::vector<EncoderInfo> encoder_candidates(AVCodecID codec_id, bool allow_hardware) {
    std::vector<EncoderInfo> out;
    for (const EncoderInfo& c : candidates(codec_id))
        if (allow_hardware || !c.hardware) out.push_back(c);
    return out;
}

std::vector<std::string> available_hardware_encoders() {
    std::vector<std::string> out;
    for (AVCodecID id : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1, AV_CODEC_ID_VP9}) {
        for (const EncoderInfo& e : candidates(id)) {
            if (e.hardware && encoder_available(e)) out.push_back(e.label);
        }
    }
    return out;
}

std::vector<AvailableEncoder> available_video_encoders() {
    std::vector<AvailableEncoder> out;
    for (AVCodecID id : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1, AV_CODEC_ID_VP9}) {
        for (const EncoderInfo& e : candidates(id)) {
            if (encoder_available(e)) out.push_back({id, e, encoder_supports_lossless(e)});
        }
    }
    return out;
}

}  // namespace syna
