#include "common/ffmpeg.h"

#include "common/util.h"

#include <algorithm>
#include <cerrno>

namespace syna {

std::string av_error_string(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

syna_result map_av_error(int err) {
    if (err >= 0) return SYNA_OK;
    switch (err) {
        case AVERROR(ENOENT): return SYNA_E_NOT_FOUND;
        case AVERROR(EACCES):
        case AVERROR(EPERM): return SYNA_E_ACCESS_DENIED;
        case AVERROR(ENOSPC): return SYNA_E_DISK_FULL;
        case AVERROR(EIO): return SYNA_E_IO;
        case AVERROR_INVALIDDATA: return SYNA_E_OPEN;
        case AVERROR_DECODER_NOT_FOUND:
        case AVERROR_DEMUXER_NOT_FOUND:
        case AVERROR_ENCODER_NOT_FOUND:
        case AVERROR_PATCHWELCOME: return SYNA_E_UNSUPPORTED;
        case AVERROR_EXIT: return SYNA_E_CANCELLED;
        default: return SYNA_E_FAIL;
    }
}

FormatPtr open_input(const std::wstring& path, bool find_stream_info, std::string* error, int* av_err) {
    AVFormatContext* raw = nullptr;
    std::string utf8 = to_utf8(path);
    int r = avformat_open_input(&raw, utf8.c_str(), nullptr, nullptr);
    if (r < 0) {
        if (error) *error = "cannot open: " + av_error_string(r);
        if (av_err) *av_err = r;
        return nullptr;
    }
    FormatPtr fmt(raw);
    if (find_stream_info) {
        r = avformat_find_stream_info(fmt.get(), nullptr);
        if (r < 0) {
            if (error) *error = "cannot read stream info: " + av_error_string(r);
            if (av_err) *av_err = r;
            return nullptr;
        }
    }
    return fmt;
}

int64_t media_duration_us(AVFormatContext* fmt) {
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) return fmt->duration;
    int64_t best = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0) best = std::max(best, av_rescale_q(st->duration, st->time_base, kMicros));
    }
    return best;
}

int best_video_stream(AVFormatContext* fmt) {
    int best = -1;
    int64_t best_area = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
        int64_t area = static_cast<int64_t>(st->codecpar->width) * st->codecpar->height;
        if (area > best_area) {
            best_area = area;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace syna
