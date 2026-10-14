#pragma once

#include "audio/audio_output.h"
#include "audio/mixer.h"
#include "media/media_info.h"
#include "media/queues.h"
#include "video/video_decoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace syna {

// Per-track playback health, for diagnostics (all times in microseconds unless noted).
struct AudioTrackStats {
    int ordinal = 0;
    std::string state;
    int64_t pts_us = INT64_MIN;   // last decoded frame
    int64_t buffered_us = 0;      // decoded audio waiting for the mixer
    double drift_ms = 0;          // smoothed timestamp drift being corrected
    int64_t discontinuities = 0;  // timestamp jumps >= 100 ms honored as gaps/overlaps
    int64_t gap_us = 0;
    int64_t overlap_us = 0;
    int64_t stretched_us = 0;
    int64_t underruns = 0;
    std::string codec;
    int sample_rate = 0;
    int channels = 0;
};

// Everything needed to play one file: demuxer, decoders, queues and mixer. Owned by the Player;
// only the active session has a pipeline. Times are on the file's timeline (container start = 0).
class Pipeline {
public:
    struct Deps {
        ID3D11Device* device = nullptr;
        AudioOutput* audio = nullptr;
        MixSettings* settings = nullptr;
        MixMeters* meters = nullptr;
        HANDLE frame_event = nullptr;   // signaled when a decoded frame is queued
    };

    static constexpr size_t kFrameQueueCapacity = 6;

    explicit Pipeline(const Deps& deps);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    syna_result open(const std::wstring& path, bool prefer_hw, std::string* error);
    void start(int64_t position_us, bool exact);
    void stop();

    // Starts a new seek generation; returns its serial. margin_us moves the demuxer seek point
    // earlier (used to retry when a keyframe turned out to be displayed after the target).
    int seek(int64_t target_us, bool exact, int64_t margin_us = 0);
    int serial() const { return serial_.load(); }
    int64_t seek_target_us() const { return seek_target_us_.load(); }

    // True once every audio track has buffered audio at the mix position (or ended).
    bool audio_ready() const;
    bool audio_finished() const { return mixer_ ? mixer_->finished() : true; }

    FrameQueue& frames() { return frames_; }
    Mixer* mixer() const { return mixer_.get(); }
    const MediaInfo& info() const { return info_; }
    int64_t duration_us() const { return info_.duration_us; }
    bool has_video() const { return video_index_ >= 0; }
    int audio_track_count() const { return static_cast<int>(tracks_.size()); }

    int decoder_backend() const { return decoder_backend_.load(); }
    std::string decoder_detail() const;
    std::vector<AudioTrackStats> audio_stats() const;
    bool source_error() const { return source_error_.load(); }
    bool hw_fallback_happened() const { return hw_fallback_.load(); }

private:
    struct SeekRequest {
        int64_t target_us = 0;
        bool exact = true;
        int serial = 0;
        int64_t margin_us = 0;
    };

    void demux_loop();
    void video_loop();
    void audio_loop();
    bool queues_full() const;
    void perform_seek(const SeekRequest& req);
    void push_eos(int serial);
    bool decode_audio_packet(AudioTrack& t, const AVPacket* packet);
    void emit_audio(AudioTrack& t, AVFrame* frame);

    Deps deps_;
    FormatPtr fmt_;
    MediaInfo info_;
    int64_t origin_us_ = 0;
    int video_index_ = -1;
    AVRational video_tb_{1, 1};
    int64_t default_frame_us_ = 16667;
    VideoDecoder video_decoder_;
    std::vector<std::unique_ptr<AudioTrack>> tracks_;
    std::vector<int> stream_to_track_;
    std::unique_ptr<Mixer> mixer_;

    UniqueHandle demux_wake_;      // seeks, aborts, consumed packets
    UniqueHandle audio_wake_;      // packets pushed, mixer consumed audio
    UniqueHandle abort_event_;
    std::unique_ptr<PacketQueue> video_packets_;
    FrameQueue frames_;

    std::thread demux_thread_;
    std::thread video_thread_;
    std::thread audio_thread_;
    std::atomic<bool> abort_{false};
    bool started_ = false;

    std::mutex seek_mutex_;
    SeekRequest pending_seek_;
    bool seek_pending_ = false;
    std::atomic<int> serial_{0};
    std::atomic<int64_t> seek_target_us_{0};

    // Parameters of the generation currently being demuxed (published before its packets).
    std::atomic<int> gen_serial_{-1};
    std::atomic<int64_t> gen_target_us_{0};
    std::atomic<bool> gen_exact_{true};
    std::atomic<int64_t> gen_margin_us_{0};
    std::atomic<int64_t> gen_seek_us_{0};
    std::atomic<int64_t> gen_audio_start_us_{0};

    bool eof_ = false;
    int read_errors_ = 0;
    std::atomic<bool> source_error_{false};
    std::atomic<int> decoder_backend_{SYNA_DECODER_NONE};
    std::atomic<bool> hw_fallback_{false};
};

}  // namespace syna
