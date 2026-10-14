#pragma once

#include "common/ffmpeg.h"
#include "jobs/job.h"

#include <string>
#include <vector>

namespace syna {

// A source audio track as the player's mixer plays it (syna_trim_track).
struct TrimTrack {
    int stream = -1;
    float gain = 1.0f;
    int64_t offset_us = 0;  // positive = later
};

struct TrimParams {
    std::wstring source;
    std::wstring destination;
    int64_t in_us = 0;   // file timeline
    int64_t out_us = 0;  // exclusive
    int mode = SYNA_TRIM_SMART;
    bool allow_hw_encode = true;
    // Video (syna_trim_request)
    int video = SYNA_TRIM_VIDEO_ORIGINAL;
    int video_codec = SYNA_VIDEO_CODEC_SOURCE;
    int video_height = 0;
    AVRational video_fps{0, 1};  // 0: source timing
    int video_quality = 0;       // H.264 CQ scale; 0: default
    int64_t video_bitrate = 0;   // bit/s
    int64_t video_max_bitrate = 0;
    int video_speed = SYNA_ENCODER_SPEED_BALANCED;
    // Audio
    int audio = SYNA_TRIM_AUDIO_MIX;
    int audio_codec = SYNA_AUDIO_CODEC_AUTO;
    int64_t audio_bitrate = 0;   // bit/s; 0: default
    int audio_sample_rate = 0;
    int audio_channels = 0;
    std::vector<TrimTrack> tracks;  // streams not listed are used as recorded
};

// Writes [in, out) of the source to destination through a temp file in the same folder. The source
// is only read. The result is re-opened and validated; the JSON report says what was actually done.
syna_result run_trim(const TrimParams& params, Job& job);

}  // namespace syna
