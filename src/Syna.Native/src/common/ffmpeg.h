#pragma once

// D3D11 must be included outside extern "C": hwcontext_d3d11va.h pulls it in and its C++ operator
// overloads cannot have C linkage.
#include "common/util.h"

#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

#include "syna_api.h"

#include <memory>
#include <string>

namespace syna {

struct FormatCloser {
    void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;

struct OutputFormatFree {
    void operator()(AVFormatContext* c) const {
        if (!c) return;
        if (c->pb && !(c->oformat->flags & AVFMT_NOFILE)) avio_closep(&c->pb);
        avformat_free_context(c);
    }
};
using OutputFormatPtr = std::unique_ptr<AVFormatContext, OutputFormatFree>;

struct CodecFree {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
using CodecPtr = std::unique_ptr<AVCodecContext, CodecFree>;

struct FrameFree {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;

struct PacketFree {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;

struct SwrFree {
    void operator()(SwrContext* s) const { swr_free(&s); }
};
using SwrPtr = std::unique_ptr<SwrContext, SwrFree>;

struct BufferUnref {
    void operator()(AVBufferRef* b) const { av_buffer_unref(&b); }
};
using BufferPtr = std::unique_ptr<AVBufferRef, BufferUnref>;

inline FramePtr make_frame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr make_packet() { return PacketPtr(av_packet_alloc()); }

inline constexpr AVRational kMicros{1, 1000000};

inline int64_t ts_to_us(int64_t ts, AVRational tb) {
    return ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, tb, kMicros);
}
inline int64_t us_to_ts(int64_t us, AVRational tb) {
    return us == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(us, kMicros, tb);
}

std::string av_error_string(int err);
syna_result map_av_error(int err);

// Opens a file for demuxing. path is UTF-16; FFmpeg receives UTF-8 (its Windows file protocol
// converts back and handles long paths).
FormatPtr open_input(const std::wstring& path, bool find_stream_info, std::string* error, int* av_err = nullptr);

int best_video_stream(AVFormatContext* fmt);

// Container duration, falling back to the longest stream when the container field is unset
// (e.g. MP4 opened without avformat_find_stream_info).
int64_t media_duration_us(AVFormatContext* fmt);

}  // namespace syna
