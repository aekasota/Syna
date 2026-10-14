#include "audio/mixer.h"

#include "common/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace syna {
namespace {

void raise_peak(std::atomic<float>& meter, float value) {
    if (value > meter.load(std::memory_order_relaxed)) meter.store(value, std::memory_order_relaxed);
}

}  // namespace

Mixer::Mixer(std::vector<AudioTrack*> tracks, MixSettings& settings, MixMeters& meters, int channels, int sample_rate,
             HANDLE wake_decoder, AudioOutput& output)
    : tracks_(std::move(tracks)),
      settings_(settings),
      meters_(meters),
      output_(output),
      channels_(channels),
      sample_rate_(sample_rate),
      wake_decoder_(wake_decoder),
      limiter_(sample_rate) {}

void Mixer::reset(int64_t media_us) {
    std::lock_guard lock(m_);
    const int64_t pos = to_samples(media_us);
    position_.store(pos);
    tempo_media_us_ = static_cast<double>(media_us);
    if (tempo_.active()) tempo_.configure(rate_, channels_, sample_rate_);
    warmup_until_ = pos + sample_rate_ / 5;
    for (AudioTrack* t : tracks_) t->primed = false;
}

bool Mixer::finished() const {
    for (AudioTrack* t : tracks_) {
        if (!t->eos.load()) return false;
        const int64_t end = t->fifo.end_pos();
        const int64_t offset = av_rescale(settings_.tracks[t->ordinal].offset_us.load(), sample_rate_, 1000000);
        if (end != INT64_MIN && end > position_.load() - offset) return false;
    }
    return true;
}

void Mixer::mix_block(float* out, int frames) {
    const size_t samples = static_cast<size_t>(frames) * channels_;
    std::memset(out, 0, sizeof(float) * samples);
    scratch_.resize(samples);
    const int64_t pos = position_.load();

    bool any_solo = false;
    for (AudioTrack* t : tracks_) any_solo |= settings_.tracks[t->ordinal].solo.load();

    for (AudioTrack* t : tracks_) {
        const TrackSettings& s = settings_.tracks[t->ordinal];
        // Trims export the same level (Syna.Core MixerState.EffectiveGain keeps this rule).
        const float target = (s.mute.load() || (any_solo && !s.solo.load())) ? 0.0f : std::max(0.0f, s.gain.load());
        const int64_t track_pos = pos - av_rescale(s.offset_us.load(), sample_rate_, 1000000);
        const int covered = t->fifo.read(scratch_.data(), frames, track_pos);
        if (covered > 0) t->primed = true;
        if (covered < frames && t->primed && !t->eos.load() && pos > warmup_until_) {
            output_.count_underrun();
            t->underruns.fetch_add(1, std::memory_order_relaxed);
        }

        const float g0 = t->current_gain;
        const float step = (target - g0) / static_cast<float>(frames);
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i) {
            const float g = g0 + step * static_cast<float>(i + 1);
            float* o = out + static_cast<size_t>(i) * channels_;
            const float* in = scratch_.data() + static_cast<size_t>(i) * channels_;
            for (int c = 0; c < channels_; ++c) {
                const float v = in[c] * g;
                o[c] += v;
                peak = std::max(peak, std::fabs(v));
            }
        }
        t->current_gain = target;
        raise_peak(meters_.track_peak[t->ordinal], peak);
    }

    const float mt = settings_.master_mute.load() ? 0.0f : std::max(0.0f, settings_.master_gain.load());
    const float m0 = master_gain_;
    const float mstep = (mt - m0) / static_cast<float>(frames);
    const bool limit = settings_.limiter.load();
    float min_env = 1.0f;
    float peak_l = 0.0f, peak_r = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float* o = out + static_cast<size_t>(i) * channels_;
        const float g = m0 + mstep * static_cast<float>(i + 1);
        float frame_peak = 0.0f;
        for (int c = 0; c < channels_; ++c) {
            o[c] *= g;
            frame_peak = std::max(frame_peak, std::fabs(o[c]));
        }
        if (limit) {
            const float env = limiter_.gain(frame_peak);
            for (int c = 0; c < channels_; ++c) o[c] *= env;
            min_env = std::min(min_env, env);
        } else {
            for (int c = 0; c < channels_; ++c) o[c] = std::clamp(o[c], -1.0f, 1.0f);
        }
        peak_l = std::max(peak_l, std::fabs(o[0]));
        peak_r = std::max(peak_r, std::fabs(o[channels_ > 1 ? 1 : 0]));
    }
    master_gain_ = mt;
    raise_peak(meters_.master_peak[0], peak_l);
    raise_peak(meters_.master_peak[1], peak_r);
    const float gr = min_env < 1.0f ? -20.0f * std::log10(min_env) : 0.0f;
    if (gr > meters_.gain_reduction_db.load(std::memory_order_relaxed)) meters_.gain_reduction_db.store(gr, std::memory_order_relaxed);

    position_.store(pos + frames);
}

void Mixer::render(float* out, int frames, AudioChunkInfo& info) {
    std::lock_guard lock(m_);
    const double requested = requested_rate_.load();
    if (requested != rate_) {
        rate_ = requested;
        tempo_media_us_ = static_cast<double>(av_rescale(position_.load(), 1000000, sample_rate_));
        if (rate_ != 1.0) {
            if (!tempo_.configure(rate_, channels_, sample_rate_)) rate_ = 1.0;
        } else {
            tempo_.release();
        }
    }

    if (rate_ == 1.0) {
        info.media_us = av_rescale(position_.load(), 1000000, sample_rate_);
        info.rate = 1.0;
        mix_block(out, frames);
    } else {
        info.media_us = static_cast<int64_t>(tempo_media_us_);
        info.rate = rate_;
        int got = 0;
        const int block = 1024;
        block_.resize(static_cast<size_t>(block) * channels_);
        for (int guard = 0; got < frames && guard < 64; ++guard) {
            got += tempo_.pull(out + static_cast<size_t>(got) * channels_, frames - got);
            if (got < frames) {
                mix_block(block_.data(), block);
                tempo_.push(block_.data(), block);
            }
        }
        if (got < frames) std::memset(out + static_cast<size_t>(got) * channels_, 0, sizeof(float) * static_cast<size_t>(frames - got) * channels_);
        tempo_media_us_ += frames * 1e6 * rate_ / sample_rate_;
    }
    if (wake_decoder_) SetEvent(wake_decoder_);
}

}  // namespace syna
