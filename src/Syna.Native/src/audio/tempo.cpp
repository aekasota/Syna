#include "audio/tempo.h"

#include "common/ffmpeg.h"
#include "common/log.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <algorithm>
#include <cstring>
#include <string>

namespace syna {

Tempo::~Tempo() { release(); }

void Tempo::release() {
    if (graph_) avfilter_graph_free(&graph_);
    src_ = nullptr;
    sink_ = nullptr;
    av_frame_free(&in_);
    av_frame_free(&out_);
    pending_.clear();
    pending_head_ = 0;
    next_pts_ = 0;
}

bool Tempo::configure(double rate, int channels, int sample_rate) {
    release();
    channels_ = channels;
    sample_rate_ = sample_rate;

    // atempo accepts 0.5..100 per instance; chain instances for slower rates.
    std::string chain;
    double r = rate;
    while (r < 0.5) {
        chain += "atempo=0.5,";
        r /= 0.5;
    }
    chain += "atempo=" + std::to_string(r);

    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    char layout_name[64] = {};
    av_channel_layout_describe(&layout, layout_name, sizeof(layout_name));
    av_channel_layout_uninit(&layout);

    graph_ = avfilter_graph_alloc();
    char args[256];
    snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=flt:channel_layout=%s:time_base=1/%d", sample_rate, layout_name, sample_rate);
    int r1 = avfilter_graph_create_filter(&src_, avfilter_get_by_name("abuffer"), "in", args, nullptr, graph_);
    int r2 = avfilter_graph_create_filter(&sink_, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph_);
    if (r1 < 0 || r2 < 0) {
        log::error("tempo", "cannot create buffer filters");
        release();
        return false;
    }
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = src_;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink_;
    inputs->pad_idx = 0;
    inputs->next = nullptr;
    int ret = avfilter_graph_parse_ptr(graph_, chain.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret >= 0) ret = avfilter_graph_config(graph_, nullptr);
    if (ret < 0) {
        log::error("tempo", "atempo graph '{}' failed: {}", chain, av_error_string(ret));
        release();
        return false;
    }
    in_ = av_frame_alloc();
    out_ = av_frame_alloc();
    log::info("tempo", "configured rate={} chain={}", rate, chain);
    return true;
}

bool Tempo::push(const float* frames, int count) {
    if (!graph_ || count <= 0) return false;
    av_frame_unref(in_);
    in_->format = AV_SAMPLE_FMT_FLT;
    in_->sample_rate = sample_rate_;
    av_channel_layout_default(&in_->ch_layout, channels_);
    in_->nb_samples = count;
    in_->pts = next_pts_;
    next_pts_ += count;
    if (av_frame_get_buffer(in_, 0) < 0) return false;
    std::memcpy(in_->data[0], frames, sizeof(float) * static_cast<size_t>(count) * channels_);
    return av_buffersrc_add_frame_flags(src_, in_, 0) >= 0;
}

int Tempo::pull(float* out, int max_frames) {
    if (!graph_) return 0;
    while ((pending_.size() - pending_head_) / channels_ < static_cast<size_t>(max_frames)) {
        int r = av_buffersink_get_frame(sink_, out_);
        if (r < 0) break;
        const float* p = reinterpret_cast<const float*>(out_->data[0]);
        pending_.insert(pending_.end(), p, p + static_cast<size_t>(out_->nb_samples) * channels_);
        av_frame_unref(out_);
    }
    const size_t avail = (pending_.size() - pending_head_) / channels_;
    const size_t n = std::min<size_t>(avail, static_cast<size_t>(max_frames));
    std::memcpy(out, pending_.data() + pending_head_, sizeof(float) * n * channels_);
    pending_head_ += n * channels_;
    if (pending_head_ == pending_.size()) {
        pending_.clear();
        pending_head_ = 0;
    }
    return static_cast<int>(n);
}

}  // namespace syna
