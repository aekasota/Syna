#pragma once

#include "audio/audio_output.h"
#include "audio/mixer.h"
#include "media/pipeline.h"
#include "renderer/renderer.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace syna {

// One playback surface. The control thread serializes commands (open, seek, step, ...); the render
// thread presents frames against the audio clock. The UI only enqueues commands and polls state.
class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool init(std::string* error);

    HRESULT attach_panel(IUnknown* panel) { return renderer_.attach_panel(panel); }
    void resize(int width, int height, float scale_x, float scale_y);
    void set_background(float r, float g, float b);

    int64_t open(const std::wstring& path, const syna_open_options& options);
    void close();
    void play();
    void pause();
    void seek(int64_t position_us, int mode);
    void step(int frames);
    void set_rate(double rate);
    void set_loop(bool enabled, int64_t a_us, int64_t b_us);
    MixSettings& settings() { return settings_; }

    void get_state(syna_player_state& out);
    int poll_events(syna_event* out, int capacity);
    syna_result save_frame(const std::wstring& path, int format);
    syna_result media_json(std::string& out) const;
    std::string diagnostics_json() const;
    const std::string& last_open_error() const { return last_open_error_; }

private:
    struct Command {
        enum class Type { Open, Close, Play, Pause, Seek, Step, Rate, Loop } type = Type::Play;
        std::wstring path;
        syna_open_options options{};
        int64_t id = 0;
        int64_t a = 0;
        int64_t b = 0;
        double rate = 1.0;
        int mode = 0;
        int n = 0;
        bool flag = false;
    };

    void post(Command c);
    void control_loop();
    void render_loop();

    void do_open(const Command& c);
    void do_close();
    void do_seek(int64_t target_us, bool exact);
    void teardown_pipeline();

    void present(VideoFrame&& f, Pipeline& p);
    void redraw();
    void begin_playback(Pipeline& p);
    void stop_playback();
    void finish_playback();
    void report_status(Pipeline& p);
    void emit(int type, int code, int64_t value);
    std::shared_ptr<Pipeline> pipeline() const;
    int64_t video_clock_us() const;
    void wait_render(DWORD ms, bool include_frames);

    Renderer renderer_;
    AudioOutput audio_;
    MixSettings settings_;
    MixMeters meters_;

    std::thread control_thread_;
    std::mutex cmd_mutex_;
    std::condition_variable cmd_cv_;
    std::deque<Command> commands_;
    bool quit_ = false;

    std::thread render_thread_;
    UniqueHandle render_event_;
    UniqueHandle frame_event_;
    UniqueHandle quit_event_;
    std::atomic<bool> render_quit_{false};

    mutable std::mutex pipeline_mutex_;
    std::shared_ptr<Pipeline> pipeline_;
    std::mutex transport_mutex_;  // orders audio start/stop against seeks

    std::atomic<int> state_{SYNA_STATE_IDLE};
    std::atomic<int64_t> open_id_{0};
    std::atomic<int64_t> next_open_id_{0};
    std::atomic<bool> want_play_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int> presented_serial_{-1};
    std::atomic<int> step_requests_{0};
    std::atomic<bool> needs_resync_{false};
    std::atomic<double> rate_{1.0};
    std::atomic<bool> loop_enabled_{false};
    std::atomic<int64_t> loop_a_{0};
    std::atomic<int64_t> loop_b_{0};
    std::atomic<bool> loop_seek_pending_{false};
    std::atomic<int64_t> duration_us_{0};
    std::atomic<bool> video_ended_{false};
    std::atomic<bool> release_views_{false};
    std::string last_open_error_;

    VideoFrame current_;  // guarded by renderer_.mutex()
    std::atomic<int64_t> current_pts_{INT64_MIN};
    std::atomic<int64_t> current_duration_{0};

    std::mutex size_mutex_;
    bool size_pending_ = false;
    int pending_w_ = 1;
    int pending_h_ = 1;
    float pending_sx_ = 1.0f;
    float pending_sy_ = 1.0f;

    std::atomic<int64_t> frames_presented_{0};
    std::atomic<int64_t> frames_dropped_{0};
    std::atomic<double> render_ms_{0};
    std::atomic<double> av_drift_ms_{0};
    std::atomic<double> presented_fps_{0};
    int64_t fps_window_start_ = 0;
    int64_t fps_window_frames_ = 0;
    std::atomic<int> decoder_{SYNA_DECODER_NONE};
    bool fallback_reported_ = false;
    bool device_lost_reported_ = false;
    bool source_error_reported_ = false;
    uint64_t audio_device_token_ = 0;
    int64_t present_lead_us_ = 8000;

    std::mutex event_mutex_;
    std::deque<syna_event> events_;
};

}  // namespace syna
