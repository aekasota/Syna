#include "trim/audio_mixdown.h"

#include "common/log.h"

extern "C" {
#include <libavutil/audio_fifo.h>
}

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

namespace syna {
namespace {

// A track that delivers nothing (not started yet, a gap, or ended early) counts as silent once
// another track is this far ahead (plus the spread of the mixer offsets), so buffering stays
// bounded and the mix never waits forever.
constexpr int64_t kMaxSkewUs = 3000000;
constexpr int64_t kBlock = 4096;  // frames mixed per step

struct EncoderChoice {
    AVCodecID id;
    const char* name;
    const char* display;
    int kbps_per_channel;  // default bitrate; 0 = lossless
    int request;           // SYNA_AUDIO_CODEC_* that asks for it; 0 = only chosen automatically
};

// After the source's own codec, in order of compatibility.
constexpr EncoderChoice kEncoders[] = {
    {AV_CODEC_ID_AAC, "aac", "AAC", 128, SYNA_AUDIO_CODEC_AAC},
    {AV_CODEC_ID_OPUS, "libopus", "Opus", 80, SYNA_AUDIO_CODEC_OPUS},
    {AV_CODEC_ID_VORBIS, "libvorbis", "Vorbis", 96, 0},
    {AV_CODEC_ID_FLAC, "flac", "FLAC", 0, SYNA_AUDIO_CODEC_FLAC},
    {AV_CODEC_ID_MP3, "libmp3lame", "MP3", 160, 0},
};

bool container_accepts(const AVOutputFormat* ofmt, AVCodecID id) {
    const int q = avformat_query_codec(ofmt, id, FF_COMPLIANCE_NORMAL);
    return q == 1 || (q < 0 && ofmt->audio_codec == id);
}

template <class T>
std::vector<T> supported(const AVCodecContext* ctx, const AVCodec* codec, AVCodecConfig cfg) {
    const void* list = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(ctx, codec, cfg, 0, &list, &n) < 0 || !list || n <= 0) return {};
    const T* p = static_cast<const T*>(list);
    return std::vector<T>(p, p + n);
}

int pick_rate(const std::vector<int>& rates, int want) {
    if (rates.empty() || std::ranges::find(rates, want) != rates.end()) return want;
    int best = 0;
    for (int r : rates)
        if (r >= want && (best == 0 || r < best)) best = r;
    return best ? best : *std::ranges::max_element(rates);
}

void pick_layout(const std::vector<AVChannelLayout>& layouts, const AVChannelLayout& want, AVChannelLayout& out) {
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    const AVChannelLayout* pick = layouts.empty() ? &want : nullptr;
    for (const auto& l : layouts)
        if (!pick && av_channel_layout_compare(&l, &want) == 0) pick = &l;
    for (const auto& l : layouts)
        if (!pick && l.nb_channels == want.nb_channels) pick = &l;
    for (const auto& l : layouts)
        if (!pick && av_channel_layout_compare(&l, &stereo) == 0) pick = &l;
    av_channel_layout_copy(&out, pick ? pick : &layouts.front());
}

// Lossless encoders get 32-bit samples (24 significant bits) so the float mix is not cut to 16 bits.
AVSampleFormat pick_format(const std::vector<AVSampleFormat>& formats, bool deep) {
    if (formats.empty()) return AV_SAMPLE_FMT_FLTP;
    if (deep)
        for (AVSampleFormat f : {AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32})
            if (std::ranges::find(formats, f) != formats.end()) return f;
    for (AVSampleFormat f : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32})
        if (std::ranges::find(formats, f) != formats.end()) return f;
    return formats.front();
}

}  // namespace

struct AudioMixdown::Track {
    int stream = -1;
    float gain = 1.0f;         // the mixer's: 0 for a muted track or one a solo silences
    int64_t offset_us = 0;     // the mixer's: positive = later
    AVRational time_base{};
    CodecPtr dec;
    TrackConverter conv;
    FramePtr frame = make_frame();
    std::vector<float> converted;
    std::vector<float> buf;    // interleaved samples covering [buf_start, buf_start + buf.size() / channels)
    int64_t buf_start = 0;     // output samples
    int64_t end = INT64_MIN;   // output sample after the last one placed
    bool done = false;         // nothing more will arrive inside the range
};

AudioMixdown::AudioMixdown() = default;

AudioMixdown::~AudioMixdown() {
    if (fifo_) av_audio_fifo_free(fifo_);
}

bool AudioMixdown::open(const AVFormatContext* in, const std::vector<TrimTrack>& tracks, const AVOutputFormat* ofmt, bool global_header,
                        const AudioEncodeSettings& settings, std::string& reason) {
    int want_rate = 0;
    AVChannelLayout want{};
    ScopeExit free_want([&] { av_channel_layout_uninit(&want); });
    for (size_t k = 0; k < tracks.size(); ++k) {
        const AVStream* st = in->streams[tracks[k].stream];
        const AVCodecParameters* par = st->codecpar;
        auto t = std::make_unique<Track>();
        t->stream = tracks[k].stream;
        t->gain = tracks[k].gain;
        t->offset_us = tracks[k].offset_us;
        t->time_base = st->time_base;
        if (const AVCodec* codec = avcodec_find_decoder(par->codec_id)) {
            t->dec.reset(avcodec_alloc_context3(codec));
            if (!t->dec || avcodec_parameters_to_context(t->dec.get(), par) < 0) t->dec.reset();
            if (t->dec) {
                t->dec->pkt_timebase = st->time_base;
                if (avcodec_open2(t->dec.get(), codec, nullptr) < 0) t->dec.reset();
            }
        }
        if (!t->dec) {
            reason = std::format("audio track {} ({}) cannot be decoded", k + 1, avcodec_get_name(par->codec_id));
            return false;
        }
        // The output keeps the best rate and the widest layout among the tracks.
        want_rate = std::max(want_rate, par->sample_rate);
        if (par->ch_layout.nb_channels > want.nb_channels) {
            av_channel_layout_uninit(&want);
            if (par->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) av_channel_layout_copy(&want, &par->ch_layout);
            else av_channel_layout_default(&want, par->ch_layout.nb_channels);
        }
        tracks_.push_back(std::move(t));
    }
    if (tracks_.empty()) {
        reason = "no audio tracks";
        return false;
    }
    if (want.nb_channels == 0) av_channel_layout_default(&want, 2);
    if (want_rate <= 0) want_rate = 48000;
    if (settings.sample_rate > 0) want_rate = settings.sample_rate;
    if (settings.channels == 1 || settings.channels == 2) {
        av_channel_layout_uninit(&want);
        av_channel_layout_default(&want, settings.channels);
    }

    // A requested codec is used or the open fails; automatic picks the source's codec first.
    std::vector<const EncoderChoice*> order;
    const AVCodecID source = in->streams[tracks.front().stream]->codecpar->codec_id;
    for (const auto& e : kEncoders)
        if (settings.codec != SYNA_AUDIO_CODEC_AUTO ? e.request == settings.codec : e.id == source) order.push_back(&e);
    if (settings.codec == SYNA_AUDIO_CODEC_AUTO)
        for (const auto& e : kEncoders)
            if (e.id != source) order.push_back(&e);
    const EncoderChoice* chosen = nullptr;
    for (const EncoderChoice* e : order) {
        if (!container_accepts(ofmt, e->id)) {
            if (settings.codec != SYNA_AUDIO_CODEC_AUTO) reason = std::format("{} audio cannot be stored in {}", e->display, ofmt->long_name ? ofmt->long_name : ofmt->name);
            continue;
        }
        const AVCodec* codec = avcodec_find_encoder_by_name(e->name);
        if (!codec) continue;
        CodecPtr ctx(avcodec_alloc_context3(codec));
        if (!ctx) continue;
        ctx->sample_rate = pick_rate(supported<int>(ctx.get(), codec, AV_CODEC_CONFIG_SAMPLE_RATE), want_rate);
        pick_layout(supported<AVChannelLayout>(ctx.get(), codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT), want, ctx->ch_layout);
        ctx->sample_fmt = pick_format(supported<AVSampleFormat>(ctx.get(), codec, AV_CODEC_CONFIG_SAMPLE_FORMAT), e->kbps_per_channel == 0);
        if (e->kbps_per_channel == 0) {
            if (av_get_bytes_per_sample(ctx->sample_fmt) == 4) ctx->bits_per_raw_sample = 24;
        } else {
            ctx->bit_rate = settings.bitrate > 0 ? settings.bitrate : static_cast<int64_t>(e->kbps_per_channel) * 1000 * ctx->ch_layout.nb_channels;
            if (e->id == AV_CODEC_ID_MP3) ctx->bit_rate = std::min<int64_t>(ctx->bit_rate, 320000);
        }
        ctx->time_base = {1, ctx->sample_rate};
        if (global_header) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int r = avcodec_open2(ctx.get(), codec, nullptr);
        if (r >= 0) {
            enc_ = std::move(ctx);
            chosen = e;
            break;
        }
        log::warn("trim", "audio encoder {} failed to open: {}", e->name, av_error_string(r));
        if (settings.codec != SYNA_AUDIO_CODEC_AUTO) reason = std::format("the {} encoder could not be opened: {}", e->display, av_error_string(r));
    }
    if (!enc_) {
        if (reason.empty()) reason = "no audio encoder is available for this output format";
        return false;
    }
    // Summed tracks, or a track the mixer boosts, can exceed full scale; the limiter only touches
    // peaks that would clip.
    limit_ = tracks_.size() > 1 || std::ranges::any_of(tracks_, [](const auto& t) { return t->gain > 1.0f; });

    channels_ = enc_->ch_layout.nb_channels;
    rate_ = enc_->sample_rate;
    const auto [early, late] = std::ranges::minmax(tracks_ | std::views::transform([](const auto& t) { return t->offset_us; }));
    skew_ = av_rescale(kMaxSkewUs + (late - early), rate_, 1000000);
    for (size_t k = 0; k < tracks_.size(); ++k) tracks_[k]->conv.set_output(enc_->ch_layout, rate_, static_cast<int>(k));
    SwrContext* s = nullptr;
    if (swr_alloc_set_opts2(&s, &enc_->ch_layout, enc_->sample_fmt, rate_, &enc_->ch_layout, AV_SAMPLE_FMT_FLT, rate_, 0, nullptr) < 0 || swr_init(s) < 0) {
        swr_free(&s);
        reason = "cannot configure the audio sample converter";
        return false;
    }
    to_encoder_.reset(s);
    fifo_ = av_audio_fifo_alloc(enc_->sample_fmt, channels_, static_cast<int>(kBlock) * 2);
    if (!fifo_) {
        reason = "out of memory";
        return false;
    }
    limiter_ = PeakLimiter(rate_);
    converted_ = make_frame();
    pkt_ = make_packet();
    std::string mixer;
    for (const auto& t : tracks_) mixer += std::format("{}stream {} gain {:.3f} offset {} ms", mixer.empty() ? "" : ", ", t->stream, t->gain, t->offset_us / 1000);
    log::info("trim", "audio {} of {} track(s) into {} ({}); {}", tracks_.size() > 1 ? "mixdown" : "encode", tracks_.size(), describe(), chosen->name, mixer);
    return true;
}

bool AudioMixdown::lossless() const { return enc_ && enc_->codec_id == AV_CODEC_ID_FLAC; }

std::string AudioMixdown::describe() const {
    if (!enc_) return {};
    const char* name = avcodec_get_name(enc_->codec_id);
    for (const auto& e : kEncoders)
        if (e.id == enc_->codec_id) name = e.display;
    char layout[64] = {};
    av_channel_layout_describe(&enc_->ch_layout, layout, sizeof(layout));
    std::string s = std::format("{} · {:g} kHz · {}", name, rate_ / 1000.0, layout);
    if (lossless()) s += enc_->bits_per_raw_sample > 0 ? std::format(" · lossless, {}-bit", enc_->bits_per_raw_sample) : " · lossless";
    else if (enc_->bit_rate > 0) s += std::format(" · {} kb/s", enc_->bit_rate / 1000);
    return s;
}

void AudioMixdown::begin(int64_t zero_us, int64_t length_us, Sink sink) {
    zero_us_ = zero_us;
    length_ = av_rescale(std::max<int64_t>(0, length_us), rate_, 1000000);
    sink_ = std::move(sink);
}

int64_t AudioMixdown::written_us() const { return rate_ > 0 ? av_rescale(next_pts_, 1000000, rate_) : 0; }

syna_result AudioMixdown::feed(const AVPacket* pkt) {
    for (auto& tp : tracks_) {
        Track& t = *tp;
        if (t.stream != pkt->stream_index) continue;
        if (t.done) return SYNA_OK;
        int r = avcodec_send_packet(t.dec.get(), pkt);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            // A damaged packet: the track goes on, and its timestamps account for the hole.
            log::debug("trim", "audio packet rejected: {}", av_error_string(r));
            return SYNA_OK;
        }
        while ((r = avcodec_receive_frame(t.dec.get(), t.frame.get())) >= 0) {
            const syna_result pr = place(t, t.frame.get());
            av_frame_unref(t.frame.get());
            if (pr != SYNA_OK) return pr;
        }
        return mix_until(ready_until());
    }
    return SYNA_OK;
}

syna_result AudioMixdown::place(Track& t, const AVFrame* frame) {
    const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    const int64_t ts_us = ts != AV_NOPTS_VALUE ? ts_to_us(ts, t.time_base) - zero_us_ + t.offset_us : INT64_MIN;
    TrackConverter::Placed p;
    if (!t.conv.convert(frame, ts_us, t.converted, p)) {
        error_ = "cannot convert an audio track for mixing";
        return SYNA_E_FAIL;
    }
    if (p.jump != 0)
        log::info("trim", "audio track stream {}: timestamp jump {:+.1f} ms at {:.3f} s", t.stream, p.jump * 1000.0 / rate_,
                  static_cast<double>(p.pos - p.jump) / rate_);
    if (p.frames > 0) append(t, p.pos, t.converted.data(), p.frames);
    return SYNA_OK;
}

void AudioMixdown::append(Track& t, int64_t pos, const float* data, int64_t frames) {
    const int64_t placed_end = pos + frames;
    if (t.end != INT64_MIN && pos < t.end) {  // overlap: what is already placed stays
        const int64_t cut = std::min(frames, t.end - pos);
        data += cut * channels_;
        frames -= cut;
        pos += cut;
    }
    if (pos < mixed_) {  // before the mix cursor: decoder pre-roll, or audio that arrived too late
        const int64_t cut = std::min(frames, mixed_ - pos);
        data += cut * channels_;
        frames -= cut;
        pos += cut;
    }
    t.end = t.end == INT64_MIN ? placed_end : std::max(t.end, placed_end);
    if (t.end >= length_) t.done = true;
    frames = std::min(frames, length_ - pos);
    if (frames <= 0) return;
    const int64_t have = static_cast<int64_t>(t.buf.size()) / channels_;
    if (have == 0) t.buf_start = pos;
    else if (pos > t.buf_start + have) t.buf.insert(t.buf.end(), static_cast<size_t>(pos - t.buf_start - have) * channels_, 0.0f);  // gap
    t.buf.insert(t.buf.end(), data, data + frames * channels_);
}

int64_t AudioMixdown::ready_until() const {
    int64_t lead = INT64_MIN;
    for (const auto& t : tracks_)
        if (t->end != INT64_MIN) lead = std::max(lead, t->end);
    if (lead == INT64_MIN) return mixed_;
    int64_t until = length_;
    for (const auto& t : tracks_) {
        if (t->done) continue;
        until = std::min(until, std::max(t->end, lead - skew_));
    }
    return until;
}

syna_result AudioMixdown::mix_until(int64_t until) {
    until = std::min(until, length_);
    while (until - mixed_ >= kBlock || (until > mixed_ && std::ranges::all_of(tracks_, [](const auto& t) { return t->done; }))) {
        const int64_t n = std::min(kBlock, until - mixed_);
        mix_.assign(static_cast<size_t>(n * channels_), 0.0f);
        for (auto& tp : tracks_) {
            Track& t = *tp;
            const int64_t have = static_cast<int64_t>(t.buf.size()) / channels_;
            const int64_t a = std::max(mixed_, t.buf_start), b = std::min(mixed_ + n, t.buf_start + have);
            if (b > a && t.gain > 0.0f) {
                const float* src = t.buf.data() + (a - t.buf_start) * channels_;
                float* dst = mix_.data() + (a - mixed_) * channels_;
                for (int64_t i = 0; i < (b - a) * channels_; ++i) dst[i] += src[i] * t.gain;
            }
            const int64_t drop = std::clamp<int64_t>(mixed_ + n - t.buf_start, 0, have);
            if (drop > 0) {
                t.buf.erase(t.buf.begin(), t.buf.begin() + static_cast<ptrdiff_t>(drop * channels_));
                t.buf_start += drop;
            }
        }
        // Tracks are summed at their mixer levels; the playback limiter keeps the sum from clipping.
        for (int64_t i = 0; limit_ && i < n; ++i) {
            float* f = mix_.data() + i * channels_;
            float peak = 0.0f;
            for (int c = 0; c < channels_; ++c) peak = std::max(peak, std::fabs(f[c]));
            const float g = limiter_.gain(peak);
            for (int c = 0; c < channels_; ++c) f[c] *= g;
        }
        converted_->nb_samples = static_cast<int>(n);
        converted_->format = enc_->sample_fmt;
        av_channel_layout_copy(&converted_->ch_layout, &enc_->ch_layout);
        converted_->sample_rate = rate_;
        if (av_frame_get_buffer(converted_.get(), 0) < 0) return SYNA_E_FAIL;
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mix_.data());
        const int got = swr_convert(to_encoder_.get(), converted_->extended_data, static_cast<int>(n), &src, static_cast<int>(n));
        if (got > 0) av_audio_fifo_write(fifo_, reinterpret_cast<void**>(converted_->extended_data), got);
        av_frame_unref(converted_.get());
        mixed_ += n;
        if (syna_result r = encode(false); r != SYNA_OK) return r;
    }
    return SYNA_OK;
}

syna_result AudioMixdown::encode(bool flush) {
    const int caps = enc_->codec->capabilities;
    const bool variable = (caps & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0 || enc_->frame_size <= 0;
    const int frame_size = variable ? static_cast<int>(kBlock) : enc_->frame_size;
    while (av_audio_fifo_size(fifo_) >= frame_size || (flush && av_audio_fifo_size(fifo_) > 0)) {
        const int n = std::min(frame_size, av_audio_fifo_size(fifo_));
        const bool pad = n < frame_size && !variable && !(caps & AV_CODEC_CAP_SMALL_LAST_FRAME);
        FramePtr f = make_frame();
        f->nb_samples = pad ? frame_size : n;
        f->format = enc_->sample_fmt;
        av_channel_layout_copy(&f->ch_layout, &enc_->ch_layout);
        f->sample_rate = rate_;
        if (av_frame_get_buffer(f.get(), 0) < 0) return SYNA_E_FAIL;
        if (pad) av_samples_set_silence(f->extended_data, 0, f->nb_samples, channels_, enc_->sample_fmt);
        av_audio_fifo_read(fifo_, reinterpret_cast<void**>(f->extended_data), n);
        f->pts = next_pts_;
        next_pts_ += n;
        if (const int r = avcodec_send_frame(enc_.get(), f.get()); r < 0) {
            error_ = "the audio encoder rejected a frame: " + av_error_string(r);
            return SYNA_E_FAIL;
        }
        if (syna_result r = drain_encoder(); r != SYNA_OK) return r;
    }
    if (!flush) return SYNA_OK;
    avcodec_send_frame(enc_.get(), nullptr);
    return drain_encoder();
}

syna_result AudioMixdown::drain_encoder() {
    while (avcodec_receive_packet(enc_.get(), pkt_.get()) >= 0) {
        const syna_result r = sink_(pkt_.get());
        av_packet_unref(pkt_.get());
        if (r != SYNA_OK) return r;
    }
    return SYNA_OK;
}

syna_result AudioMixdown::finish() {
    for (auto& tp : tracks_) {
        Track& t = *tp;
        avcodec_send_packet(t.dec.get(), nullptr);
        while (avcodec_receive_frame(t.dec.get(), t.frame.get()) >= 0) {
            const syna_result r = place(t, t.frame.get());
            av_frame_unref(t.frame.get());
            if (r != SYNA_OK) return r;
        }
        int64_t pos = 0;
        if (const int n = t.conv.drain(t.converted, pos); n > 0) append(t, pos, t.converted.data(), n);
        t.done = true;
    }
    // Ends where the longest track ends, or at OUT.
    int64_t lead = mixed_;
    for (const auto& t : tracks_)
        if (t->end != INT64_MIN) lead = std::max(lead, t->end);
    if (syna_result r = mix_until(lead); r != SYNA_OK) return r;
    return encode(true);
}

}  // namespace syna
