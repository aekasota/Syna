#pragma once

#include "common/util.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

struct IMMDeviceEnumerator;
struct IAudioClient;
struct IAudioRenderClient;
struct IAudioClock;

namespace syna {

struct AudioChunkInfo {
    int64_t media_us = 0;  // timeline time of the first frame produced
    double rate = 1.0;     // media seconds per output second
};

class AudioSource {
public:
    virtual ~AudioSource() = default;
    // Produces `frames` interleaved float frames in the format given to AudioOutput::set_source.
    virtual void render(float* out, int frames, AudioChunkInfo& info) = 0;
};

// Single WASAPI shared-mode output stream, owned by one thread, and the playback master clock.
// When no device is available (or for video-only media) a null sink consumes audio at wall-clock
// pace through the same code path, so the clock never changes implementation.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void start();

    // Format a source must produce. Stable for the lifetime of the output: device changes are
    // absorbed by Windows' converter (AUTOCONVERTPCM).
    int channels() const { return stream_channels_; }
    int sample_rate() const { return stream_rate_; }

    // Attaches/detaches the source. Detaching waits for an in-flight render call to finish.
    void set_source(AudioSource* source, bool null_sink);

    // Synchronous with respect to the clock.
    void play();
    void pause();
    void flush(int64_t media_us);

    // Master clock: timeline time of the sample currently audible.
    int64_t media_time_us() const;
    bool running() const { return running_.load(); }

    bool device_ok() const { return device_ok_.load(); }
    bool device_changed_since(uint64_t& token) const;
    double buffer_ms() const { return buffer_ms_.load(); }
    double queued_ms() const { return queued_ms_.load(); }
    int64_t underruns() const { return underruns_.load(); }
    void count_underrun() { underruns_.fetch_add(1); }
    // Times the device consumed everything queued while running: an audible gap made by the
    // output side (late wake-ups), as opposed to a track the decoder could not fill.
    int64_t starvations() const { return starvations_.load(); }

private:
    struct Segment {
        int64_t frame;
        int64_t media_us;
        double rate;
    };
    enum class Cmd { None, Play, Pause, Flush };

    void thread_main();
    bool open_device();
    void close_device();
    void fill_device();
    void fill_null_sink();
    void apply_command();
    void record_chunk(int64_t frames, const AudioChunkInfo& info);
    void render_from_source(float* out, int frames, AudioChunkInfo& info);
    void reset_clock_locked(int64_t media_us);

    class Notifier;
    Notifier* notifier_ = nullptr;

    std::thread thread_;
    UniqueHandle quit_event_;
    UniqueHandle command_event_;
    UniqueHandle command_done_event_;
    UniqueHandle device_event_;
    UniqueHandle buffer_event_;

    // Owned by the audio thread.
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    IAudioClock* audio_clock_ = nullptr;
    uint64_t clock_frequency_ = 0;
    uint32_t buffer_frames_ = 0;
    int64_t null_start_qpc_ = 0;
    int64_t null_start_frames_ = 0;
    int64_t next_retry_qpc_ = 0;

    int stream_channels_ = 2;
    int stream_rate_ = 48000;

    std::mutex command_mutex_;  // serializes play/pause/flush callers
    Cmd pending_cmd_ = Cmd::None;
    int64_t pending_flush_us_ = 0;

    std::mutex source_mutex_;
    AudioSource* source_ = nullptr;
    std::atomic<bool> null_sink_requested_{false};
    bool format_fixed_ = false;

    mutable std::mutex clock_mutex_;
    std::deque<Segment> segments_;
    int64_t written_frames_ = 0;
    int64_t played_frames_ = 0;
    int64_t played_qpc_ = 0;
    int64_t idle_media_us_ = 0;
    int64_t expected_media_us_ = INT64_MIN;
    double last_rate_ = 1.0;

    std::atomic<bool> playing_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> device_ok_{false};
    std::atomic<bool> device_change_pending_{false};
    std::atomic<uint64_t> device_generation_{0};
    std::atomic<double> buffer_ms_{0};
    std::atomic<double> queued_ms_{0};
    std::atomic<int64_t> underruns_{0};
    std::atomic<int64_t> starvations_{0};
};

}  // namespace syna
