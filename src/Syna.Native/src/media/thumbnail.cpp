#include "media/thumbnail.h"

#include "common/ffmpeg.h"
#include "common/log.h"
#include "image/image_writer.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <vector>

namespace syna {
namespace {

int sws_colorspace(AVColorSpace cs, int height) {
    switch (cs) {
        case AVCOL_SPC_BT709: return SWS_CS_ITU709;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
        case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
        case AVCOL_SPC_FCC: return SWS_CS_FCC;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
        default: return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

}  // namespace

syna_result make_thumbnail(const std::wstring& path, const std::wstring& out_jpeg, int max_width, int64_t at_us) {
    std::string err;
    int av_err = 0;
    FormatPtr fmt = open_input(path, false, &err, &av_err);
    if (!fmt) {
        set_last_error(err);
        return av_err ? map_av_error(av_err) : SYNA_E_OPEN;
    }
    const int vi = best_video_stream(fmt.get());
    if (vi < 0) {
        set_last_error("no video stream");
        return SYNA_E_NO_VIDEO;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream* st = fmt->streams[vi];
    st->discard = AVDISCARD_NONKEY;

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        set_last_error("unsupported codec");
        return SYNA_E_UNSUPPORTED;
    }
    CodecPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    ctx->thread_count = 1;
    ctx->skip_frame = AVDISCARD_NONKEY;
    if (int r = avcodec_open2(ctx.get(), codec, nullptr); r < 0) {
        set_last_error("decoder: " + av_error_string(r));
        return SYNA_E_UNSUPPORTED;
    }

    const int64_t start = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
    if (at_us < 0) at_us = std::min<int64_t>(media_duration_us(fmt.get()) / 10, 3000000);
    if (at_us > 0) {
        const int64_t ts = av_rescale_q_rnd(at_us + start, kMicros, st->time_base, AV_ROUND_DOWN);
        avformat_seek_file(fmt.get(), vi, INT64_MIN, ts, ts, 0);
    }

    PacketPtr pkt = make_packet();
    FramePtr frame = make_frame();
    bool got = false;
    for (int packets = 0; !got && packets < 600;) {
        int r = av_read_frame(fmt.get(), pkt.get());
        if (r < 0) {
            avcodec_send_packet(ctx.get(), nullptr);
        } else {
            if (pkt->stream_index != vi) {
                av_packet_unref(pkt.get());
                continue;
            }
            ++packets;
            avcodec_send_packet(ctx.get(), pkt.get());
            av_packet_unref(pkt.get());
        }
        if (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) got = true;
        if (r < 0 && !got) break;
    }
    if (!got) {
        set_last_error("no decodable keyframe");
        return SYNA_E_UNSUPPORTED;
    }

    const double sar = frame->sample_aspect_ratio.num > 0 ? av_q2d(frame->sample_aspect_ratio) : 1.0;
    const double display_w = frame->width * sar;
    const int out_w = std::max(2, static_cast<int>(std::min<double>(max_width, display_w)) & ~1);
    const int out_h = std::max(2, static_cast<int>(out_w * frame->height / display_w + 0.5) & ~1);
    SwsContext* sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), out_w, out_h,
                                     AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr);
    if (!sws) return SYNA_E_FAIL;
    const int* coeffs = sws_getCoefficients(sws_colorspace(frame->colorspace, frame->height));
    const int src_full = frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    sws_setColorspaceDetails(sws, coeffs, src_full, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    std::vector<uint8_t> bgra(static_cast<size_t>(out_w) * out_h * 4);
    uint8_t* dst[4] = {bgra.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {out_w * 4, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
    sws_freeContext(sws);
    return write_image(out_jpeg, bgra.data(), out_w, out_h, out_w * 4, true, 0.85f);
}

}  // namespace syna
