#include "audio/track_converter.h"

#include "common/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace syna {
namespace {

constexpr int64_t kHardDiscontinuityUs = 100000;
constexpr double kStretchStartUs = 5000;
constexpr double kStretchStopUs = 1000;
constexpr double kMaxStretch = 0.002;     // 0.2 %, about 3.5 cents: inaudible
constexpr double kDriftSmoothing = 0.02;  // per frame, about one second of AAC frames

}  // namespace

TrackConverter::~TrackConverter() {
    av_channel_layout_uninit(&in_layout_);
    av_channel_layout_uninit(&out_layout_);
}

void TrackConverter::set_output(const AVChannelLayout& layout, int sample_rate, int log_id) {
    av_channel_layout_uninit(&out_layout_);
    av_channel_layout_copy(&out_layout_, &layout);
    out_rate_ = sample_rate;
    log_id_ = log_id;
    swr_.reset();
    reset();
}

void TrackConverter::reset() {
    if (swr_) swr_init(swr_.get());  // drops buffered input and any stretch in progress
    next_pos_ = INT64_MIN;
    drift_ = 0;
    stretching_ = false;
}

bool TrackConverter::convert(const AVFrame* frame, int64_t ts_us, std::vector<float>& out, Placed& placed) {
    placed = {};
    if (!swr_ || frame->sample_rate != in_rate_ || frame->format != in_format_ || av_channel_layout_compare(&frame->ch_layout, &in_layout_) != 0) {
        SwrContext* s = nullptr;
        int r = swr_alloc_set_opts2(&s, &out_layout_, AV_SAMPLE_FMT_FLT, out_rate_, &frame->ch_layout, static_cast<AVSampleFormat>(frame->format),
                                    frame->sample_rate, 0, nullptr);
        // Resample even at equal rates, so drift correction never has to re-initialize it mid-stream.
        if (r >= 0) av_opt_set_int(s, "swr_flags", SWR_FLAG_RESAMPLE, 0);
        if (r < 0 || swr_init(s) < 0) {
            swr_free(&s);
            return false;
        }
        if (swr_) log::info("audio", "track {}: input format changed ({} Hz)", log_id_, frame->sample_rate);
        swr_.reset(s);
        stretching_ = false;
        in_rate_ = frame->sample_rate;
        in_format_ = frame->format;
        av_channel_layout_uninit(&in_layout_);
        av_channel_layout_copy(&in_layout_, &frame->ch_layout);
    }

    // Where the timestamp puts this frame's first output sample.
    int64_t stamped = INT64_MIN;
    if (ts_us != INT64_MIN) stamped = av_rescale(ts_us, out_rate_, 1000000) - swr_get_delay(swr_.get(), out_rate_);

    if (next_pos_ == INT64_MIN) {
        placed.pos = stamped != INT64_MIN ? stamped : 0;  // anchor after open or seek
        drift_ = 0;
        stretching_ = false;
    } else {
        placed.pos = next_pos_;
        if (stamped != INT64_MIN) {
            const int64_t diff = stamped - next_pos_;
            if (std::llabs(diff) >= av_rescale(kHardDiscontinuityUs, out_rate_, 1000000)) {
                placed.pos = stamped;  // a real gap or overlap in the source
                placed.jump = diff;
                drift_ = 0;
                stretching_ = false;
            } else {
                drift_ += (static_cast<double>(diff) - drift_) * kDriftSmoothing;
            }
        }
    }
    const double drift_us = drift_ * 1e6 / out_rate_;
    if (std::fabs(drift_us) > kStretchStartUs) stretching_ = true;
    else if (std::fabs(drift_us) < kStretchStopUs) stretching_ = false;
    if (stretching_) {
        // Positive drift: the timestamps place this audio later than it plays, so lengthen it.
        const int nominal = static_cast<int>(av_rescale(frame->nb_samples, out_rate_, frame->sample_rate));
        const int delta = std::max(1, static_cast<int>(nominal * kMaxStretch)) * (drift_ > 0 ? 1 : -1);
        if (nominal > 0 && swr_set_compensation(swr_.get(), delta, nominal) >= 0) placed.stretch = delta;
    }

    const int max_out = swr_get_out_samples(swr_.get(), frame->nb_samples) + 64;  // + stretch headroom
    out.resize(static_cast<size_t>(max_out) * out_layout_.nb_channels);
    uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());
    const int n = swr_convert(swr_.get(), &dst, max_out, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (n > 0) {
        placed.frames = n;
        next_pos_ = placed.pos + n;
    }
    return true;
}

int TrackConverter::drain(std::vector<float>& out, int64_t& pos) {
    if (!swr_ || next_pos_ == INT64_MIN) return 0;
    const int max_out = 4096;
    out.resize(static_cast<size_t>(max_out) * out_layout_.nb_channels);
    uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());
    const int n = swr_convert(swr_.get(), &dst, max_out, nullptr, 0);
    if (n <= 0) return 0;
    pos = next_pos_;
    next_pos_ += n;
    return n;
}

}  // namespace syna
