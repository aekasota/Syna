#pragma once

#include "common/ffmpeg.h"

#include <string>
#include <vector>

namespace syna {

struct AudioTrackInfo {
    int stream_index = -1;
    std::string codec;
    std::string profile;
    int sample_rate = 0;
    int channels = 0;
    std::string channel_layout;
    std::string sample_format;
    int bits_per_sample = 0;
    int64_t bit_rate = 0;
    std::string title;          // from container metadata, empty if none
    std::string language;
    std::string label;          // display label (title, or a suggestion)
    bool label_is_suggestion = false;
    int64_t start_us = 0;
    int64_t duration_us = 0;
};

struct VideoInfo {
    int stream_index = -1;
    std::string codec;
    std::string codec_long;
    std::string profile;
    int level = 0;
    int width = 0;
    int height = 0;
    AVRational sar{1, 1};
    std::string pixel_format;
    int bit_depth = 0;
    std::string chroma;         // 4:2:0 / 4:2:2 / 4:4:4
    std::string color_range;
    std::string color_space;
    std::string color_primaries;
    std::string color_transfer;
    std::string hdr;            // "", "HDR10 (PQ)", "HLG"
    double fps_avg = 0;
    double fps_nominal = 0;     // r_frame_rate
    bool vfr = false;           // timestamps deviate from a constant cadence
    double frame_interval_min_ms = 0;
    double frame_interval_max_ms = 0;
    int64_t frame_count = 0;
    int64_t bit_rate = 0;
    int has_b_frames = 0;
    double rotation = 0;
    double keyframe_interval_s = 0;
    std::vector<int64_t> keyframes_us;  // FULL mode only
    int64_t start_us = 0;
};

struct MediaInfo {
    std::wstring path;
    std::string container;
    std::string container_long;
    int64_t duration_us = 0;
    int64_t size_bytes = 0;
    int64_t bit_rate = 0;
    int64_t start_us = 0;
    bool has_video = false;
    VideoInfo video;
    std::vector<AudioTrackInfo> audio;
};

// Fills MediaInfo from an already opened format context.
void describe_media(AVFormatContext* fmt, const std::wstring& path, bool full, MediaInfo& out);
std::string media_info_json(const MediaInfo& info);

// Display label for an audio track, from metadata when present, otherwise a marked suggestion.
void infer_track_labels(std::vector<AudioTrackInfo>& tracks);

syna_result probe_media(const std::wstring& path, bool full, MediaInfo& out);

}  // namespace syna
