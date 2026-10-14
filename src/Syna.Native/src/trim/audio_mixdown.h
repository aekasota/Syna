#pragma once

#include "audio/limiter.h"
#include "audio/track_converter.h"
#include "common/ffmpeg.h"
#include "syna_api.h"
#include "trim/trim.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVAudioFifo;

namespace syna {

// Encoded audio for a trim output (syna_trim_request audio_* fields).
struct AudioEncodeSettings {
    int codec = SYNA_AUDIO_CODEC_AUTO;  // AUTO: the source's codec when it can be encoded, then the most compatible one
    int64_t bitrate = 0;                // bit/s; 0: the codec's default
    int sample_rate = 0;                // 0: the highest among the tracks
    int channels = 0;                   // 0: the widest layout among the tracks; 1: mono; 2: stereo
};

// Decodes one or more audio tracks of a source into one encoded stream (trim output). Each track is
// decoded and placed on the timeline exactly as playback places it (TrackConverter), moved by its
// mixer offset and scaled by its mixer gain. Several tracks are summed through the playback limiter,
// as is a single track the mixer boosts; a single track at or below its recorded level passes as it is.
class AudioMixdown {
public:
    using Sink = std::function<syna_result(AVPacket*)>;  // receives packets in the encoder time base

    AudioMixdown();
    ~AudioMixdown();
    AudioMixdown(const AudioMixdown&) = delete;
    AudioMixdown& operator=(const AudioMixdown&) = delete;

    // Opens a decoder per track and an encoder the output container accepts. On failure `reason`
    // says why.
    bool open(const AVFormatContext* in, const std::vector<TrimTrack>& tracks, const AVOutputFormat* ofmt, bool global_header,
              const AudioEncodeSettings& settings, std::string& reason);

    const AVCodecContext* encoder() const { return enc_.get(); }
    bool lossless() const;
    std::string describe() const;  // "AAC · 48 kHz · stereo · 256 kb/s"

    // Output sample 0 is the source time `zero_us` (container time base, microseconds); the mix
    // is `length_us` long at most.
    void begin(int64_t zero_us, int64_t length_us, Sink sink);
    syna_result feed(const AVPacket* pkt);  // a packet of one of the mixed streams
    syna_result finish();                   // drains decoders, mixes what is left, flushes the encoder

    int64_t written_us() const;
    const std::string& error() const { return error_; }

private:
    struct Track;

    syna_result place(Track& t, const AVFrame* frame);
    void append(Track& t, int64_t pos, const float* data, int64_t frames);
    int64_t ready_until() const;
    syna_result mix_until(int64_t end);
    syna_result encode(bool flush);
    syna_result drain_encoder();

    std::vector<std::unique_ptr<Track>> tracks_;
    CodecPtr enc_;
    SwrPtr to_encoder_;
    AVAudioFifo* fifo_ = nullptr;
    PeakLimiter limiter_;
    bool limit_ = true;   // a mix of several tracks, or a boosted one; otherwise encoded as it is
    int64_t skew_ = 0;    // samples a track may trail the leading one before it counts as silent
    Sink sink_;
    std::string error_;
    int channels_ = 0;
    int rate_ = 0;
    int64_t zero_us_ = 0;
    int64_t length_ = 0;  // samples
    int64_t mixed_ = 0;   // samples mixed so far
    int64_t next_pts_ = 0;
    std::vector<float> mix_;
    FramePtr converted_;
    PacketPtr pkt_;
};

}  // namespace syna
