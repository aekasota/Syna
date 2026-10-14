#include "media/player.h"

#include "common/json.h"
#include "common/log.h"
#include "image/image_writer.h"

#include <avrt.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstring>

namespace syna {

Player::Player() {
    render_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    frame_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    quit_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

Player::~Player() {
    {
        std::lock_guard lock(cmd_mutex_);
        quit_ = true;
    }
    cmd_cv_.notify_all();
    if (control_thread_.joinable()) control_thread_.join();
    render_quit_.store(true);
    SetEvent(quit_event_.get());
    if (render_thread_.joinable()) render_thread_.join();
    teardown_pipeline();
    std::lock_guard lock(renderer_.mutex());
    current_ = VideoFrame{};
}

bool Player::init(std::string* error) {
    if (!renderer_.init(error)) return false;
    audio_.start();
    control_thread_ = std::thread(&Player::control_loop, this);
    render_thread_ = std::thread(&Player::render_loop, this);
    return true;
}

void Player::emit(int type, int code, int64_t value) {
    std::lock_guard lock(event_mutex_);
    if (events_.size() > 256) events_.pop_front();
    events_.push_back(syna_event{type, code, open_id_.load(), value});
}

int Player::poll_events(syna_event* out, int capacity) {
    std::lock_guard lock(event_mutex_);
    int n = 0;
    while (n < capacity && !events_.empty()) {
        out[n++] = events_.front();
        events_.pop_front();
    }
    return n;
}

std::shared_ptr<Pipeline> Player::pipeline() const {
    std::lock_guard lock(pipeline_mutex_);
    return pipeline_;
}

int64_t Player::video_clock_us() const {
    const int64_t t = audio_.media_time_us() + settings_.av_offset_us.load();
    return std::clamp<int64_t>(t, 0, std::max<int64_t>(duration_us_.load(), 0));
}

void Player::post(Command c) {
    {
        std::lock_guard lock(cmd_mutex_);
        commands_.push_back(std::move(c));
    }
    cmd_cv_.notify_one();
}

void Player::resize(int width, int height, float scale_x, float scale_y) {
    {
        std::lock_guard lock(size_mutex_);
        size_pending_ = true;
        pending_w_ = width;
        pending_h_ = height;
        pending_sx_ = scale_x;
        pending_sy_ = scale_y;
    }
    SetEvent(render_event_.get());
}

void Player::set_background(float r, float g, float b) {
    {
        std::lock_guard lock(renderer_.mutex());
        renderer_.set_background(r, g, b);
    }
    std::lock_guard lock(size_mutex_);
    size_pending_ = true;  // forces a redraw
    SetEvent(render_event_.get());
}

int64_t Player::open(const std::wstring& path, const syna_open_options& options) {
    Command c;
    c.type = Command::Type::Open;
    c.path = path;
    c.options = options;
    c.id = next_open_id_.fetch_add(1) + 1;
    post(std::move(c));
    return c.id;
}

void Player::close() { post({Command::Type::Close}); }
void Player::play() { post({Command::Type::Play}); }
void Player::pause() { post({Command::Type::Pause}); }

void Player::seek(int64_t position_us, int mode) {
    Command c;
    c.type = Command::Type::Seek;
    c.a = position_us;
    c.mode = mode;
    post(std::move(c));
}

void Player::step(int frames) {
    Command c;
    c.type = Command::Type::Step;
    c.n = frames;
    post(std::move(c));
}

void Player::set_rate(double rate) {
    Command c;
    c.type = Command::Type::Rate;
    c.rate = std::clamp(rate, 0.25, 4.0);
    post(std::move(c));
}

void Player::set_loop(bool enabled, int64_t a_us, int64_t b_us) {
    Command c;
    c.type = Command::Type::Loop;
    c.flag = enabled;
    c.a = a_us;
    c.b = b_us;
    post(std::move(c));
}

void Player::control_loop() {
    set_thread_name(L"Syna control");
    for (;;) {
        Command c;
        {
            std::unique_lock lock(cmd_mutex_);
            cmd_cv_.wait(lock, [&] { return quit_ || !commands_.empty(); });
            if (quit_) return;
            c = std::move(commands_.front());
            commands_.pop_front();
            // Scrubbing produces bursts of seeks: only the latest matters.
            while (c.type == Command::Type::Seek && !commands_.empty() && commands_.front().type == Command::Type::Seek) {
                c = std::move(commands_.front());
                commands_.pop_front();
            }
        }
        auto p = pipeline();
        switch (c.type) {
            case Command::Type::Open: do_open(c); break;
            case Command::Type::Close: do_close(); break;
            case Command::Type::Play:
                if (!p) break;
                if (state_.load() == SYNA_STATE_ENDED) {
                    do_seek(loop_enabled_.load() ? loop_a_.load() : 0, true);
                } else if (needs_resync_.exchange(false) && current_pts_.load() != INT64_MIN) {
                    do_seek(current_pts_.load(), true);
                }
                want_play_.store(true);
                SetEvent(render_event_.get());
                break;
            case Command::Type::Pause:
                want_play_.store(false);
                SetEvent(render_event_.get());
                break;
            case Command::Type::Seek:
                if (p) do_seek(c.a, c.mode == SYNA_SEEK_EXACT);
                break;
            case Command::Type::Step:
                if (!p) break;
                want_play_.store(false);
                if (c.n > 0) {
                    step_requests_.fetch_add(c.n);
                    needs_resync_.store(true);
                } else if (current_pts_.load() != INT64_MIN) {
                    // Previous frame = last frame displayed before the current one.
                    do_seek(current_pts_.load() - 1, true);
                }
                SetEvent(render_event_.get());
                break;
            case Command::Type::Rate:
                rate_.store(c.rate);
                if (p && p->mixer()) p->mixer()->set_rate(c.rate);
                break;
            case Command::Type::Loop:
                loop_a_.store(std::max<int64_t>(0, c.a));
                loop_b_.store(c.b);
                loop_enabled_.store(c.flag);
                loop_seek_pending_.store(false);
                break;
        }
    }
}

void Player::teardown_pipeline() {
    {
        std::lock_guard t(transport_mutex_);
        audio_.flush(0);
        playing_.store(false);
    }
    audio_.set_source(nullptr, true);
    std::shared_ptr<Pipeline> old;
    {
        std::lock_guard lock(pipeline_mutex_);
        old = std::move(pipeline_);
    }
    want_play_.store(false);
    if (old) old->stop();
    release_views_.store(true);
}

void Player::do_open(const Command& c) {
    teardown_pipeline();
    open_id_.store(c.id);
    state_.store(SYNA_STATE_OPENING);
    fallback_reported_ = false;
    source_error_reported_ = false;
    video_ended_.store(false);
    step_requests_.store(0);
    needs_resync_.store(false);
    loop_seek_pending_.store(false);
    current_pts_.store(INT64_MIN);
    frames_dropped_.store(0);
    frames_presented_.store(0);
    decoder_.store(SYNA_DECODER_NONE);

    Pipeline::Deps deps;
    deps.device = renderer_.device();
    deps.audio = &audio_;
    deps.settings = &settings_;
    deps.meters = &meters_;
    deps.frame_event = frame_event_.get();
    auto p = std::make_shared<Pipeline>(deps);
    const int64_t t0 = qpc_now();
    std::string err;
    syna_result r = p->open(c.path, c.options.hw_decode != 0, &err);
    if (r != SYNA_OK) {
        last_open_error_ = err;
        log::error("player", "open failed code={} error=\"{}\" path=\"{}\"", r, err, to_utf8(c.path));
        state_.store(SYNA_STATE_ERROR);
        emit(SYNA_EVENT_OPEN_FAILED, r, 0);
        return;
    }
    duration_us_.store(p->duration_us());
    const int64_t start = std::clamp<int64_t>(c.options.start_us, 0, std::max<int64_t>(0, p->duration_us() - 1));
    if (p->mixer()) p->mixer()->set_rate(rate_.load());
    audio_.set_source(p->mixer(), p->audio_track_count() == 0);
    {
        std::lock_guard t(transport_mutex_);
        audio_.flush(start - settings_.av_offset_us.load());
        playing_.store(false);
        presented_serial_.store(-1);
        p->start(start, true);
        std::lock_guard lock(pipeline_mutex_);
        pipeline_ = p;
    }
    want_play_.store(c.options.autoplay != 0);
    state_.store(SYNA_STATE_PAUSED);
    log::info("player", "open id={} took_ms={:.1f} start_us={}", c.id, qpc_to_ms(qpc_now() - t0), start);
    emit(SYNA_EVENT_OPENED, 0, p->duration_us());
    SetEvent(render_event_.get());
}

void Player::do_close() {
    teardown_pipeline();
    state_.store(SYNA_STATE_IDLE);
    duration_us_.store(0);
    current_pts_.store(INT64_MIN);
    {
        std::lock_guard lock(renderer_.mutex());
        current_ = VideoFrame{};
    }
    std::lock_guard lock(size_mutex_);
    size_pending_ = true;  // redraw background
    SetEvent(render_event_.get());
}

void Player::do_seek(int64_t target_us, bool exact) {
    auto p = pipeline();
    if (!p) return;
    target_us = std::clamp<int64_t>(target_us, 0, std::max<int64_t>(0, p->duration_us() - 1));
    {
        std::lock_guard t(transport_mutex_);
        audio_.flush(target_us - settings_.av_offset_us.load());
        playing_.store(false);
        p->seek(target_us, exact);
    }
    video_ended_.store(false);
    step_requests_.store(0);
    loop_seek_pending_.store(false);
    if (state_.load() == SYNA_STATE_ENDED) state_.store(SYNA_STATE_PAUSED);
    SetEvent(render_event_.get());
}

void Player::present(VideoFrame&& f, Pipeline& p) {
    double ms = 0;
    bool ok;
    {
        std::lock_guard lock(renderer_.mutex());
        if (HANDLE w = renderer_.frame_latency_waitable()) WaitForSingleObjectEx(w, 100, TRUE);
        ok = renderer_.present(f.frame.get(), &ms);
        current_ = std::move(f);
    }
    current_pts_.store(current_.pts_us);
    current_duration_.store(current_.duration_us);
    if (!ok) return;
    render_ms_.store(ms);
    frames_presented_.fetch_add(1);
    if (playing_.load()) av_drift_ms_.store(static_cast<double>(current_.pts_us - video_clock_us()) / 1000.0);
    decoder_.store(p.decoder_backend());
    const int64_t now = qpc_now();
    if (fps_window_start_ == 0) fps_window_start_ = now;
    ++fps_window_frames_;
    const double elapsed = qpc_to_ms(now - fps_window_start_);
    if (elapsed >= 1000.0) {
        presented_fps_.store(fps_window_frames_ * 1000.0 / elapsed);
        fps_window_start_ = now;
        fps_window_frames_ = 0;
    }
}

void Player::redraw() {
    std::lock_guard lock(renderer_.mutex());
    renderer_.present(current_.frame.get(), nullptr);
}

void Player::begin_playback(Pipeline& p) {
    // Give the audio decoders a moment so playback starts with sound, but never block for long.
    const int64_t deadline = qpc_now() + us_to_qpc(250000);
    const int serial = presented_serial_.load();
    while (!p.audio_ready() && qpc_now() < deadline && !render_quit_.load() && p.serial() == serial && want_play_.load()) Sleep(2);
    std::lock_guard t(transport_mutex_);
    if (p.serial() != presented_serial_.load() || !want_play_.load() || playing_.load()) return;
    fps_window_start_ = 0;
    fps_window_frames_ = 0;
    audio_.play();
    playing_.store(true);
    state_.store(SYNA_STATE_PLAYING);
}

void Player::stop_playback() {
    std::lock_guard t(transport_mutex_);
    if (!playing_.load()) return;
    audio_.pause();
    playing_.store(false);
    if (state_.load() == SYNA_STATE_PLAYING) state_.store(SYNA_STATE_PAUSED);
    presented_fps_.store(0);
}

void Player::finish_playback() {
    if (loop_enabled_.load()) {
        if (!loop_seek_pending_.exchange(true)) {
            Command c;
            c.type = Command::Type::Seek;
            c.a = loop_a_.load();
            c.mode = SYNA_SEEK_EXACT;
            post(std::move(c));
            emit(SYNA_EVENT_LOOPED, 0, loop_a_.load());
        }
        return;
    }
    stop_playback();
    want_play_.store(false);
    state_.store(SYNA_STATE_ENDED);
    emit(SYNA_EVENT_ENDED, 0, current_pts_.load());
}

void Player::report_status(Pipeline& p) {
    if (!fallback_reported_ && p.hw_fallback_happened()) {
        fallback_reported_ = true;
        emit(SYNA_EVENT_DECODER_FALLBACK, 0, 0);
    }
    if (!source_error_reported_ && p.source_error()) {
        source_error_reported_ = true;
        emit(SYNA_EVENT_SOURCE_ERROR, SYNA_E_IO, 0);
    }
    if (audio_.device_changed_since(audio_device_token_)) emit(SYNA_EVENT_AUDIO_DEVICE, audio_.device_ok() ? 1 : 0, 0);
}

void Player::wait_render(DWORD ms, bool include_frames) {
    HANDLE handles[3] = {quit_event_.get(), render_event_.get(), frame_event_.get()};
    WaitForMultipleObjects(include_frames ? 3 : 2, handles, FALSE, ms);
}

void Player::render_loop() {
    set_thread_name(L"Syna render");
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Playback", &task_index);
    DWM_TIMING_INFO timing = {};
    timing.cbSize = sizeof(timing);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0)
        present_lead_us_ = qpc_to_us(static_cast<int64_t>(timing.qpcRefreshPeriod)) / 2;
    PreciseWaiter waiter;

    while (!render_quit_.load()) {
        bool resize = false;
        int w = 1, h = 1;
        float sx = 1, sy = 1;
        {
            std::lock_guard lock(size_mutex_);
            if (size_pending_) {
                resize = true;
                size_pending_ = false;
                w = pending_w_;
                h = pending_h_;
                sx = pending_sx_;
                sy = pending_sy_;
            }
        }
        if (resize) {
            {
                std::lock_guard lock(renderer_.mutex());
                renderer_.apply_size(w, h, sx, sy);
            }
            redraw();
        }
        if (release_views_.exchange(false)) {
            std::lock_guard lock(renderer_.mutex());
            renderer_.release_frame_resources();
        }
        if (renderer_.device_lost()) {
            if (!device_lost_reported_) {
                device_lost_reported_ = true;
                state_.store(SYNA_STATE_ERROR);
                emit(SYNA_EVENT_DEVICE_LOST, SYNA_E_DEVICE, 0);
            }
            wait_render(250, false);
            continue;
        }

        auto p = pipeline();
        if (!p) {
            wait_render(250, false);
            continue;
        }
        report_status(*p);

        const int serial = p->serial();
        int64_t pts = 0, dur = 0;
        int fserial = 0;
        bool eos = false;

        if (presented_serial_.load() != serial) {
            // Waiting for the first frame of the latest seek generation.
            p->frames().drop_stale(serial);
            if (!p->has_video()) {
                presented_serial_.store(serial);
                emit(SYNA_EVENT_SEEK_DONE, 0, p->seek_target_us());
                continue;
            }
            if (p->frames().peek(pts, dur, fserial, eos) && fserial == serial) {
                VideoFrame f;
                p->frames().pop(f);
                if (f.eos) {
                    video_ended_.store(true);
                } else {
                    present(std::move(f), *p);
                    video_ended_.store(false);
                }
                presented_serial_.store(serial);
                emit(SYNA_EVENT_SEEK_DONE, 0, current_pts_.load());
                continue;
            }
            wait_render(10, true);
            continue;
        }

        if (want_play_.load() && !playing_.load() && state_.load() != SYNA_STATE_ENDED) {
            begin_playback(*p);
            continue;
        }
        if (!want_play_.load() && playing_.load()) {
            stop_playback();
            continue;
        }

        if (playing_.load()) {
            const int64_t now = video_clock_us();
            if (loop_enabled_.load() && !loop_seek_pending_.load()) {
                const int64_t a = loop_a_.load();
                const int64_t b = loop_b_.load() > a ? loop_b_.load() : duration_us_.load();
                if (now >= b) {
                    finish_playback();
                    continue;
                }
            }
            VideoFrame due;
            bool have = false;
            int dropped = 0;
            while (p->frames().peek(pts, dur, fserial, eos)) {
                if (fserial != serial) {
                    p->frames().drop_stale(serial);
                    continue;
                }
                if (eos) {
                    VideoFrame f;
                    p->frames().pop(f);
                    video_ended_.store(true);
                    break;
                }
                if (pts > now + present_lead_us_) break;
                VideoFrame f;
                p->frames().pop(f);
                if (have) ++dropped;
                due = std::move(f);
                have = true;
            }
            if (dropped) frames_dropped_.fetch_add(dropped);
            if (have) {
                present(std::move(due), *p);
                continue;
            }
            const bool video_done = video_ended_.load() || !p->has_video();
            if (video_done && p->audio_finished()) {
                const int64_t end = std::max(current_pts_.load() + current_duration_.load(), p->has_video() ? 0 : duration_us_.load());
                if (now >= end || now >= duration_us_.load() - 1) {
                    finish_playback();
                    continue;
                }
            }
            if (p->frames().peek(pts, dur, fserial, eos) && fserial == serial && !eos) {
                const double rate = std::max(0.1, rate_.load());
                const int64_t wait_us = std::clamp<int64_t>(static_cast<int64_t>((pts - present_lead_us_ - now) / rate), 0, 20000);
                waiter.wait_us(wait_us, render_event_.get());
            } else {
                wait_render(10, true);
            }
            continue;
        }

        // Paused.
        if (step_requests_.load() > 0) {
            if (p->frames().peek(pts, dur, fserial, eos)) {
                if (fserial != serial) {
                    p->frames().drop_stale(serial);
                    continue;
                }
                VideoFrame f;
                p->frames().pop(f);
                step_requests_.fetch_sub(1);
                if (f.eos) {
                    step_requests_.store(0);
                    video_ended_.store(true);
                } else {
                    present(std::move(f), *p);
                }
                emit(SYNA_EVENT_SEEK_DONE, 0, current_pts_.load());
                continue;
            }
            if (video_ended_.load()) {
                step_requests_.store(0);
                continue;
            }
            wait_render(20, true);
            continue;
        }
        wait_render(250, false);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void Player::get_state(syna_player_state& out) {
    std::memset(&out, 0, sizeof(out));
    auto p = pipeline();
    out.state = state_.load();
    out.decoder = decoder_.load();
    out.open_id = open_id_.load();
    out.duration_us = duration_us_.load();
    out.frame_pts_us = current_pts_.load();
    out.frame_duration_us = current_duration_.load();
    out.rate = rate_.load();
    out.seeking = p && presented_serial_.load() != p->serial();
    if (out.seeking) out.position_us = p->seek_target_us();
    else if (playing_.load()) out.position_us = video_clock_us();
    else out.position_us = out.frame_pts_us != INT64_MIN ? out.frame_pts_us : 0;
    out.audio_tracks = p ? p->audio_track_count() : 0;
    out.audio_device_ok = audio_.device_ok() ? 1 : 0;
    out.loop_enabled = loop_enabled_.load() ? 1 : 0;
    for (int i = 0; i < SYNA_MAX_AUDIO_TRACKS; ++i) out.track_peak[i] = meters_.track_peak[i].exchange(0.0f);
    out.master_peak[0] = meters_.master_peak[0].exchange(0.0f);
    out.master_peak[1] = meters_.master_peak[1].exchange(0.0f);
    out.limiter_gain_reduction_db = meters_.gain_reduction_db.exchange(0.0f);
    out.frames_presented = frames_presented_.load();
    out.frames_dropped = frames_dropped_.load();
    out.audio_underruns = audio_.underruns();
    out.presented_fps = playing_.load() ? presented_fps_.load() : 0.0;
    out.av_drift_ms = av_drift_ms_.load();
    out.audio_buffer_ms = audio_.buffer_ms();
    out.audio_latency_ms = audio_.queued_ms();
    out.render_ms = render_ms_.load();
    out.video_queue = p ? static_cast<int32_t>(p->frames().size()) : 0;
    out.video_queue_max = static_cast<int32_t>(Pipeline::kFrameQueueCapacity);
}

syna_result Player::save_frame(const std::wstring& path, int format) {
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    {
        std::lock_guard lock(renderer_.mutex());
        if (!current_.frame) {
            set_last_error("no frame on screen");
            return SYNA_E_NOT_FOUND;
        }
        if (!renderer_.render_to_bgra(current_.frame.get(), pixels, w, h)) {
            set_last_error("frame readback failed");
            return SYNA_E_FAIL;
        }
    }
    return write_image(path, pixels.data(), w, h, w * 4, format == 1, 0.95f);
}

std::string Player::diagnostics_json() const {
    auto p = pipeline();
    JsonWriter w;
    w.begin_object();
    w.field("decoder", p ? p->decoder_detail() : std::string());
    w.field("gpu", renderer_.adapter().name);
    w.field("gpuVendor", vendor_name(renderer_.adapter().vendor_id));
    w.field("renderer", "Direct3D 11 composition swap chain");
    w.field("audioFormat", std::format("{} Hz · {} ch · float", audio_.sample_rate(), audio_.channels()));
    w.field("audioDevice", audio_.device_ok());
    w.field("audioBufferMs", audio_.buffer_ms());
    w.field("audioQueuedMs", audio_.queued_ms());
    w.field("audioUnderruns", audio_.underruns());
    w.field("audioStarvations", audio_.starvations());
    w.key("audioTracks").begin_array();
    if (p) {
        for (const AudioTrackStats& s : p->audio_stats()) {
            w.begin_object();
            w.field("ordinal", s.ordinal);
            w.field("state", s.state);
            if (s.pts_us != INT64_MIN) w.field("ptsUs", s.pts_us);
            w.field("bufferedUs", s.buffered_us);
            w.field("driftMs", s.drift_ms);
            w.field("discontinuities", s.discontinuities);
            w.field("gapUs", s.gap_us);
            w.field("overlapUs", s.overlap_us);
            w.field("stretchedUs", s.stretched_us);
            w.field("underruns", s.underruns);
            w.field("codec", s.codec);
            w.field("sampleRate", s.sample_rate);
            w.field("channels", s.channels);
            w.end_object();
        }
    }
    w.end_array();
    w.end_object();
    return w.str();
}

syna_result Player::media_json(std::string& out) const {
    auto p = pipeline();
    if (!p) return SYNA_E_NOT_FOUND;
    out = media_info_json(p->info());
    return SYNA_OK;
}

}  // namespace syna
