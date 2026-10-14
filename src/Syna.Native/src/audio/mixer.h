#pragma once

#include "audio/audio_output.h"
#include "audio/limiter.h"
#include "audio/sample_fifo.h"
#include "audio/tempo.h"
#include "audio/track_converter.h"
#include "common/ffmpeg.h"
#include "media/queues.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace syna {

struct TrackSettings {
    std::atomic<float> gain{1.0f};
    std::atomic<bool> mute{false};
    std::atomic<bool> solo{false};
    std::atomic<int64_t> offset_us{0};  // positive = this track plays later
};

// Owned by the player, so mixer settings survive opening another file.
struct MixSettings {
    TrackSettings tracks[SYNA_MAX_AUDIO_TRACKS];
    std::atomic<float> master_gain{1.0f};
    std::atomic<bool> master_mute{false};
    std::atomic<bool> limiter{true};
    std::atomic<int64_t> av_offset_us{0};  // positive = audio delayed relative to video
};

struct MixMeters {
    std::atomic<float> track_peak[SYNA_MAX_AUDIO_TRACKS] = {};
    std::atomic<float> master_peak[2] = {};
    std::atomic<float> gain_reduction_db{0};
};

// One decoded audio stream feeding the mixer.
struct AudioTrack {
    explicit AudioTrack(HANDLE consumed, HANDLE pushed) : packets(consumed, pushed) {}

    int ordinal = 0;
    AVStream* stream = nullptr;
    CodecPtr decoder;
    TrackConverter converter;
    PacketQueue packets;
    SampleFifo fifo;
    int serial = -1;
    std::atomic<bool> eos{false};
    int64_t drop_before = INT64_MIN;     // timeline samples; exact seek trims decoded audio before this
    std::vector<float> convert;
    FramePtr frame;
    float current_gain = 1.0f;          // mixer thread only
    bool primed = false;                // mixer thread only

    // Diagnostics, written by the decode and mixer threads.
    std::atomic<int64_t> last_pts_us{INT64_MIN};
    std::atomic<double> drift_ms{0};
    std::atomic<int64_t> discontinuities{0};  // timestamp jumps >= 100 ms, honored as gaps/overlaps
    std::atomic<int64_t> gap_samples{0};      // silence inserted for those gaps
    std::atomic<int64_t> overlap_samples{0};  // samples trimmed for those overlaps
    std::atomic<int64_t> stretched_samples{0};
    std::atomic<int64_t> underruns{0};        // mix blocks this track could not fill
};

// Mixes all tracks into the output format and acts as the AudioOutput's source.
class Mixer final : public AudioSource {
public:
    Mixer(std::vector<AudioTrack*> tracks, MixSettings& settings, MixMeters& meters, int channels, int sample_rate,
          HANDLE wake_decoder, AudioOutput& output);

    void render(float* out, int frames, AudioChunkInfo& info) override;

    // Positions the mix at a timeline time (us). Call while output is flushed/paused.
    void reset(int64_t media_us);
    void set_rate(double rate) { requested_rate_.store(rate); }

    int64_t position_samples() const { return position_.load(); }
    int64_t to_samples(int64_t us) const { return av_rescale(us, sample_rate_, 1000000); }
    int sample_rate() const { return sample_rate_; }
    int channels() const { return channels_; }
    bool finished() const;

private:
    void mix_block(float* out, int frames);

    std::vector<AudioTrack*> tracks_;
    MixSettings& settings_;
    MixMeters& meters_;
    AudioOutput& output_;
    const int channels_;
    const int sample_rate_;
    HANDLE wake_decoder_;

    std::mutex m_;
    std::atomic<int64_t> position_{0};
    std::atomic<double> requested_rate_{1.0};
    double rate_ = 1.0;
    Tempo tempo_;
    double tempo_media_us_ = 0;
    int64_t warmup_until_ = 0;
    float master_gain_ = 1.0f;
    PeakLimiter limiter_;
    std::vector<float> scratch_;
    std::vector<float> block_;
};

}  // namespace syna
