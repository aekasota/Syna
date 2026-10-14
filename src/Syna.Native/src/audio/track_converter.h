#pragma once

#include "common/ffmpeg.h"

#include <cstdint>
#include <vector>

namespace syna {

// Turns one audio track's decoded frames into interleaved float at a fixed rate and layout and
// places them on the media timeline. Playback (Pipeline) and export (AudioMixdown) both use it, so
// an exported mix lines the tracks up exactly as playback does.
//
// Recorders stamp audio packets with their capture clock rather than the sample count: ShadowPlay
// tracks wander up to +-15 ms around it, with single packets 4 ms early or late. Decoded audio is
// therefore treated as continuous. Timestamps anchor a track after open or seek and reveal real gaps
// or overlaps (at least 100 ms, the threshold FFmpeg's aresample uses too); slow drift between the
// two clocks is absorbed by stretching the track through its resampler (at most 0.2 %).
class TrackConverter {
public:
    struct Placed {
        int64_t pos = 0;   // timeline sample (at the output rate) of the first converted frame
        int frames = 0;    // frames written to the output buffer
        int64_t jump = 0;  // honored timestamp discontinuity in samples: > 0 gap, < 0 overlap
        int stretch = 0;   // samples added (> 0) or removed (< 0) by drift correction
    };

    TrackConverter() = default;
    ~TrackConverter();
    TrackConverter(const TrackConverter&) = delete;
    TrackConverter& operator=(const TrackConverter&) = delete;

    // Output format and the track number used in log lines. Resets the track.
    void set_output(const AVChannelLayout& layout, int sample_rate, int log_id);

    // Converts a decoded frame whose first sample is at timeline time `ts_us` (INT64_MIN when the
    // frame has no timestamp). Returns false when no resampler can be configured for it.
    bool convert(const AVFrame* frame, int64_t ts_us, std::vector<float>& out, Placed& placed);

    // Drains the resampler at end of stream; the frames continue the track.
    int drain(std::vector<float>& out, int64_t& pos);

    // Forgets resampler history and the timeline anchor (after a seek).
    void reset();

    int64_t next_pos() const { return next_pos_; }
    double drift_ms() const { return out_rate_ > 0 ? drift_ * 1000.0 / out_rate_ : 0.0; }

private:
    SwrPtr swr_;
    int in_rate_ = 0;
    int in_format_ = -1;
    AVChannelLayout in_layout_{};
    AVChannelLayout out_layout_{};
    int out_rate_ = 0;
    int log_id_ = 0;
    int64_t next_pos_ = INT64_MIN;  // timeline sample where the next converted sample belongs
    double drift_ = 0;              // smoothed (timestamp position - next_pos_), samples
    bool stretching_ = false;       // drift correction active
};

}  // namespace syna
