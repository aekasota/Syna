#include "media/pipeline.h"

#include "common/log.h"

#include <algorithm>
#include <cstdlib>

namespace syna {
namespace {

int interrupt_callback(void* opaque) {
    return static_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}

}  // namespace

Pipeline::Pipeline(const Deps& deps) : deps_(deps), frames_(kFrameQueueCapacity, deps.frame_event) {
    demux_wake_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    audio_wake_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    abort_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    video_packets_ = std::make_unique<PacketQueue>(demux_wake_.get(), nullptr);
}

Pipeline::~Pipeline() { stop(); }

syna_result Pipeline::open(const std::wstring& path, bool prefer_hw, std::string* error) {
    int av_err = 0;
    fmt_ = open_input(path, true, error, &av_err);
    if (!fmt_) return av_err ? map_av_error(av_err) : SYNA_E_OPEN;
    fmt_->interrupt_callback.callback = &interrupt_callback;
    fmt_->interrupt_callback.opaque = &abort_;
    describe_media(fmt_.get(), path, true, info_);
    origin_us_ = fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time : 0;

    for (unsigned i = 0; i < fmt_->nb_streams; ++i) fmt_->streams[i]->discard = AVDISCARD_ALL;
    stream_to_track_.assign(fmt_->nb_streams, -1);

    video_index_ = info_.has_video ? info_.video.stream_index : -1;
    if (video_index_ >= 0) {
        AVStream* vs = fmt_->streams[video_index_];
        vs->discard = AVDISCARD_DEFAULT;
        video_tb_ = vs->time_base;
        if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0)
            default_frame_us_ = av_rescale(1000000, vs->avg_frame_rate.den, vs->avg_frame_rate.num);
        std::string verr;
        if (!video_decoder_.open(vs, deps_.device, prefer_hw, static_cast<int>(kFrameQueueCapacity) + 4, 0, &verr)) {
            if (error) *error = verr;
            return SYNA_E_UNSUPPORTED;
        }
        if (video_decoder_.hw_attempted()) decoder_backend_.store(SYNA_DECODER_D3D11VA);
        else decoder_backend_.store(SYNA_DECODER_SOFTWARE);
    }

    for (const AudioTrackInfo& a : info_.audio) {
        if (tracks_.size() >= SYNA_MAX_AUDIO_TRACKS) break;
        AVStream* st = fmt_->streams[a.stream_index];
        auto t = std::make_unique<AudioTrack>(demux_wake_.get(), audio_wake_.get());
        t->ordinal = static_cast<int>(tracks_.size());
        t->stream = st;
        t->frame = make_frame();
        t->fifo.configure(deps_.audio->channels(), deps_.audio->sample_rate() / 500);
        AVChannelLayout mix_layout;
        av_channel_layout_default(&mix_layout, deps_.audio->channels());
        t->converter.set_output(mix_layout, deps_.audio->sample_rate(), t->ordinal);
        if (const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id)) {
            CodecPtr ctx(avcodec_alloc_context3(codec));
            if (ctx && avcodec_parameters_to_context(ctx.get(), st->codecpar) >= 0) {
                ctx->pkt_timebase = st->time_base;
                ctx->thread_count = 1;
                int r = avcodec_open2(ctx.get(), codec, nullptr);
                if (r >= 0) t->decoder = std::move(ctx);
                else log::warn("pipeline", "audio track {} decoder open failed: {}", t->ordinal, av_error_string(r));
            }
        } else {
            log::warn("pipeline", "audio track {} codec {} unsupported", t->ordinal, avcodec_get_name(st->codecpar->codec_id));
        }
        if (t->decoder) {
            st->discard = AVDISCARD_DEFAULT;
            stream_to_track_[static_cast<size_t>(a.stream_index)] = t->ordinal;
        } else {
            t->eos.store(true);
        }
        tracks_.push_back(std::move(t));
    }

    if (video_index_ < 0 && tracks_.empty()) {
        if (error) *error = "no playable video or audio stream";
        return SYNA_E_UNSUPPORTED;
    }
    std::vector<AudioTrack*> raw;
    for (auto& t : tracks_) raw.push_back(t.get());
    mixer_ = std::make_unique<Mixer>(std::move(raw), *deps_.settings, *deps_.meters, deps_.audio->channels(),
                                     deps_.audio->sample_rate(), audio_wake_.get(), *deps_.audio);
    log::info("pipeline", "opened \"{}\" container={} video={} audio_tracks={} duration_ms={}", to_utf8(path), info_.container,
              info_.has_video ? info_.video.codec : "none", tracks_.size(), info_.duration_us / 1000);
    return SYNA_OK;
}

void Pipeline::start(int64_t position_us, bool exact) {
    seek(position_us, exact);
    demux_thread_ = std::thread(&Pipeline::demux_loop, this);
    if (video_index_ >= 0) video_thread_ = std::thread(&Pipeline::video_loop, this);
    if (!tracks_.empty()) audio_thread_ = std::thread(&Pipeline::audio_loop, this);
    started_ = true;
}

void Pipeline::stop() {
    abort_.store(true);
    SetEvent(abort_event_.get());
    video_packets_->abort();
    for (auto& t : tracks_) t->packets.abort();
    frames_.abort();
    SetEvent(demux_wake_.get());
    SetEvent(audio_wake_.get());
    if (demux_thread_.joinable()) demux_thread_.join();
    if (video_thread_.joinable()) video_thread_.join();
    if (!audio_thread_.joinable()) return;
    audio_thread_.join();
    for (const AudioTrackStats& s : audio_stats()) {
        log::info("pipeline", "audio track {} summary: jumps={} silence={}ms trimmed={}ms stretched={}ms drift={:.1f}ms underruns={}", s.ordinal,
                  s.discontinuities, s.gap_us / 1000, s.overlap_us / 1000, s.stretched_us / 1000, s.drift_ms, s.underruns);
    }
    if (deps_.audio)
        log::info("pipeline", "audio output totals since start: underruns={} device_starvations={}", deps_.audio->underruns(), deps_.audio->starvations());
}

int Pipeline::seek(int64_t target_us, bool exact, int64_t margin_us) {
    std::lock_guard lock(seek_mutex_);
    const int s = serial_.fetch_add(1) + 1;
    pending_seek_ = {target_us, exact, s, margin_us};
    seek_pending_ = true;
    seek_target_us_.store(target_us);
    if (mixer_) mixer_->reset(target_us - deps_.settings->av_offset_us.load());
    frames_.wake();
    SetEvent(demux_wake_.get());
    SetEvent(audio_wake_.get());
    return s;
}

bool Pipeline::audio_ready() const {
    if (!mixer_) return true;
    const int s = serial_.load();
    const int64_t margin = mixer_->to_samples(60000);
    for (const auto& t : tracks_) {
        if (!t->decoder || t->eos.load()) continue;
        if (t->serial != s) return false;
        const int64_t offset = mixer_->to_samples(deps_.settings->tracks[t->ordinal].offset_us.load());
        const int64_t need = mixer_->position_samples() - offset + margin;
        const int64_t end = t->fifo.end_pos();
        if (end == INT64_MIN || end < need) return false;
    }
    return true;
}

std::vector<AudioTrackStats> Pipeline::audio_stats() const {
    std::vector<AudioTrackStats> out;
    for (const auto& t : tracks_) {
        AudioTrackStats s;
        s.ordinal = t->ordinal;
        s.state = !t->decoder ? "no decoder" : t->eos.load() ? "ended" : "decoding";
        s.pts_us = t->last_pts_us.load(std::memory_order_relaxed);
        s.buffered_us = mixer_ ? av_rescale(t->fifo.buffered_frames(), 1000000, mixer_->sample_rate()) : 0;
        s.drift_ms = t->drift_ms.load(std::memory_order_relaxed);
        const int rate = mixer_ ? mixer_->sample_rate() : 48000;
        s.discontinuities = t->discontinuities.load(std::memory_order_relaxed);
        s.gap_us = av_rescale(t->gap_samples.load(std::memory_order_relaxed), 1000000, rate);
        s.overlap_us = av_rescale(t->overlap_samples.load(std::memory_order_relaxed), 1000000, rate);
        s.stretched_us = av_rescale(t->stretched_samples.load(std::memory_order_relaxed), 1000000, rate);
        s.underruns = t->underruns.load(std::memory_order_relaxed);
        const AVCodecParameters* par = t->stream->codecpar;
        s.codec = avcodec_get_name(par->codec_id);
        s.sample_rate = par->sample_rate;
        s.channels = par->ch_layout.nb_channels;
        out.push_back(std::move(s));
    }
    return out;
}

std::string Pipeline::decoder_detail() const {
    if (video_index_ < 0) return "";
    const std::string codec = video_decoder_.codec_name();
    if (decoder_backend_.load() == SYNA_DECODER_D3D11VA)
        return "D3D11VA (" + codec + (video_decoder_.shader_bound() ? ", zero-copy" : ", GPU copy") + ")";
    std::string reason = video_decoder_.hw_failure();
    return "Software (" + codec + ")" + (reason.empty() ? "" : ": " + reason);
}

bool Pipeline::queues_full() const {
    int64_t bytes = video_packets_->bytes();
    bool audio_ok = true;
    for (const auto& t : tracks_) {
        bytes += t->packets.bytes();
        if (t->decoder && !t->eos.load() && t->packets.duration_us() < 1000000) audio_ok = false;
    }
    if (bytes > (96ll << 20)) return true;
    const bool video_ok = video_index_ < 0 || video_packets_->count() >= 48;
    return video_ok && audio_ok;
}

void Pipeline::push_eos(int serial) {
    if (video_index_ >= 0) video_packets_->push(nullptr, serial, 0);
    for (auto& t : tracks_) {
        if (t->decoder) t->packets.push(nullptr, serial, 0);
    }
}

void Pipeline::perform_seek(const SeekRequest& req) {
    const MixSettings& s = *deps_.settings;
    const int64_t audio_start = req.target_us - s.av_offset_us.load();
    int64_t earliest = req.target_us;
    for (const auto& t : tracks_) earliest = std::min(earliest, audio_start - s.tracks[t->ordinal].offset_us.load());
    // Containers index keyframes by DTS; with frame reordering a keyframe whose DTS precedes the target
    // can still be displayed after it, so step back by the reorder delay.
    int64_t margin = req.margin_us;
    if (info_.video.has_b_frames > 0) margin += (info_.video.has_b_frames + 2) * default_frame_us_;
    const int64_t seek_us = std::max<int64_t>(0, earliest - margin) + origin_us_;
    gen_seek_us_.store(seek_us - origin_us_);

    int r;
    if (video_index_ >= 0) {
        const int64_t ts = av_rescale_q_rnd(seek_us, kMicros, video_tb_, AV_ROUND_DOWN);
        r = avformat_seek_file(fmt_.get(), video_index_, INT64_MIN, ts, ts, 0);
        if (r < 0) r = av_seek_frame(fmt_.get(), video_index_, ts, AVSEEK_FLAG_BACKWARD);
    } else {
        r = avformat_seek_file(fmt_.get(), -1, INT64_MIN, seek_us, seek_us, 0);
    }
    if (r < 0) {
        log::warn("pipeline", "seek to {} us failed ({}); restarting from the beginning", seek_us, av_error_string(r));
        av_seek_frame(fmt_.get(), -1, origin_us_, AVSEEK_FLAG_BACKWARD);
    }
    video_packets_->flush();
    for (auto& t : tracks_) t->packets.flush();
    gen_target_us_.store(req.target_us);
    gen_exact_.store(req.exact);
    gen_margin_us_.store(req.margin_us);
    gen_audio_start_us_.store(audio_start);
    gen_serial_.store(req.serial);
    eof_ = false;
    read_errors_ = 0;
    SetEvent(audio_wake_.get());
}

void Pipeline::demux_loop() {
    set_thread_name(L"Syna demux");
    PacketPtr pkt = make_packet();
    HANDLE waits[2] = {demux_wake_.get(), abort_event_.get()};
    while (!abort_.load()) {
        SeekRequest req;
        bool do_seek = false;
        {
            std::lock_guard lock(seek_mutex_);
            if (seek_pending_) {
                req = pending_seek_;
                seek_pending_ = false;
                do_seek = true;
            }
        }
        if (do_seek) {
            perform_seek(req);
            continue;
        }
        if (eof_ || queues_full()) {
            WaitForMultipleObjects(2, waits, FALSE, eof_ ? 200 : 50);
            continue;
        }
        int r = av_read_frame(fmt_.get(), pkt.get());
        if (r < 0) {
            if (abort_.load()) break;
            if (r == AVERROR(EAGAIN)) {
                Sleep(2);
                continue;
            }
            if (r != AVERROR_EOF) {
                log::warn("pipeline", "read error: {}", av_error_string(r));
                if (++read_errors_ < 3 && !avio_feof(fmt_->pb)) continue;
                source_error_.store(true);
            }
            push_eos(gen_serial_.load());
            eof_ = true;
            continue;
        }
        read_errors_ = 0;
        const int si = pkt->stream_index;
        AVStream* st = fmt_->streams[si];
        const int64_t dur = pkt->duration > 0 ? ts_to_us(pkt->duration, st->time_base) : 0;
        if (si == video_index_) {
            AVPacket* p = av_packet_alloc();
            av_packet_move_ref(p, pkt.get());
            video_packets_->push(p, gen_serial_.load(), dur > 0 ? dur : default_frame_us_);
        } else if (si < static_cast<int>(stream_to_track_.size()) && stream_to_track_[static_cast<size_t>(si)] >= 0) {
            AVPacket* p = av_packet_alloc();
            av_packet_move_ref(p, pkt.get());
            tracks_[static_cast<size_t>(stream_to_track_[static_cast<size_t>(si)])]->packets.push(p, gen_serial_.load(), dur);
        } else {
            av_packet_unref(pkt.get());
        }
    }
}

void Pipeline::video_loop() {
    set_thread_name(L"Syna video decode");
    FramePtr frame = make_frame();
    VideoFrame held;
    bool have_held = false;
    int serial = -1;
    bool seeking = false;
    bool exact = true;
    bool first_of_generation = false;
    int64_t target = 0;
    int64_t last_pts = 0;
    bool logged_backend = false;

    auto push = [&](VideoFrame&& vf) {
        frames_.push(std::move(vf), serial_);
    };

    while (!abort_.load()) {
        QueuedPacket qp;
        if (!video_packets_->pop(qp, std::chrono::milliseconds(100))) continue;
        PacketPtr owned(qp.packet);
        if (qp.serial != serial_.load()) continue;  // superseded by a newer seek
        if (qp.serial != serial) {
            video_decoder_.flush();
            serial = qp.serial;
            have_held = false;
            held = VideoFrame{};
            seeking = true;
            first_of_generation = true;
            exact = gen_exact_.load();
            target = gen_target_us_.load();
        }
        int r = video_decoder_.send(owned.get());
        if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) log::debug("pipeline", "video packet rejected: {}", av_error_string(r));

        for (;;) {
            r = video_decoder_.receive(frame.get());
            if (r < 0) break;
            if (serial != serial_.load()) {
                av_frame_unref(frame.get());
                break;
            }
            const bool hw = frame->format == AV_PIX_FMT_D3D11;
            if (!logged_backend) {
                logged_backend = true;
                decoder_backend_.store(hw ? SYNA_DECODER_D3D11VA : SYNA_DECODER_SOFTWARE);
                if (!hw && video_decoder_.hw_attempted()) hw_fallback_.store(true);
                log::info("pipeline", "video frames produced by {} ({})", hw ? "hardware" : "software", decoder_detail());
            }
            int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            VideoFrame vf;
            vf.pts_us = ts != AV_NOPTS_VALUE ? ts_to_us(ts, video_tb_) - origin_us_ : last_pts + default_frame_us_;
            vf.duration_us = frame->duration > 0 ? ts_to_us(frame->duration, video_tb_) : default_frame_us_;
            vf.serial = serial;
            vf.frame = make_frame();
            av_frame_move_ref(vf.frame.get(), frame.get());
            last_pts = vf.pts_us;

            const bool first = first_of_generation;
            first_of_generation = false;
            if (seeking && exact) {
                if (vf.pts_us <= target) {
                    held = std::move(vf);
                    have_held = true;
                    continue;
                }
                if (first && !have_held && gen_seek_us_.load() > 0) {
                    // Landed on a keyframe displayed after the target: retry from further back.
                    const int64_t margin = std::max<int64_t>(1000000, gen_margin_us_.load() * 2);
                    if (margin <= 64000000) {
                        log::debug("pipeline", "seek overshoot pts={} target={}; retrying with margin {} us", vf.pts_us, target, margin);
                        seek(target, true, margin);
                        continue;
                    }
                }
                seeking = false;
                if (have_held) {
                    have_held = false;
                    push(std::move(held));
                }
                push(std::move(vf));
            } else {
                seeking = false;
                push(std::move(vf));
            }
        }
        if (!owned || r == AVERROR_EOF) {
            if (serial != serial_.load()) continue;
            if (have_held) {
                have_held = false;
                push(std::move(held));
            }
            VideoFrame eos;
            eos.eos = true;
            eos.serial = serial;
            eos.pts_us = last_pts;
            push(std::move(eos));
            video_decoder_.flush();
        }
    }
}

void Pipeline::emit_audio(AudioTrack& t, AVFrame* frame) {
    const int rate = mixer_->sample_rate();
    const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    int64_t ts_us = INT64_MIN;
    if (ts != AV_NOPTS_VALUE) {
        ts_us = ts_to_us(ts, t.stream->time_base) - origin_us_;
        t.last_pts_us.store(ts_us, std::memory_order_relaxed);
    }
    TrackConverter::Placed p;
    if (!t.converter.convert(frame, ts_us, t.convert, p)) {
        log::error("pipeline", "audio track {}: cannot configure resampler", t.ordinal);
        return;
    }
    if (p.jump != 0) {
        // A real gap or overlap in the source: the FIFO fills it with silence or trims it.
        if (t.discontinuities.fetch_add(1, std::memory_order_relaxed) < 20)
            log::info("pipeline", "audio track {}: timestamp jump {:+.1f} ms at {:.3f} s", t.ordinal, p.jump * 1000.0 / rate,
                      static_cast<double>(p.pos - p.jump) / rate);
        (p.jump > 0 ? t.gap_samples : t.overlap_samples).fetch_add(std::llabs(p.jump), std::memory_order_relaxed);
    }
    if (p.stretch != 0) t.stretched_samples.fetch_add(std::abs(p.stretch), std::memory_order_relaxed);
    t.drift_ms.store(t.converter.drift_ms(), std::memory_order_relaxed);
    if (p.frames <= 0) return;
    const int ch = mixer_->channels();
    int skip = 0;
    if (t.drop_before != INT64_MIN && p.pos < t.drop_before) skip = static_cast<int>(std::min<int64_t>(p.frames, t.drop_before - p.pos));
    if (skip < p.frames) t.fifo.write(t.convert.data() + static_cast<size_t>(skip) * ch, p.frames - skip, p.pos + skip);
}

bool Pipeline::decode_audio_packet(AudioTrack& t, const AVPacket* packet) {
    int r = avcodec_send_packet(t.decoder.get(), packet);
    if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF) log::debug("pipeline", "audio packet rejected: {}", av_error_string(r));
    while ((r = avcodec_receive_frame(t.decoder.get(), t.frame.get())) >= 0) {
        emit_audio(t, t.frame.get());
        av_frame_unref(t.frame.get());
    }
    return r == AVERROR_EOF;
}

void Pipeline::audio_loop() {
    set_thread_name(L"Syna audio decode");
    HANDLE waits[2] = {audio_wake_.get(), abort_event_.get()};
    const int64_t ahead = mixer_->to_samples(400000);
    while (!abort_.load()) {
        bool worked = false;
        const int current = serial_.load();
        for (auto& tp : tracks_) {
            AudioTrack& t = *tp;
            if (!t.decoder) continue;
            const int64_t offset = mixer_->to_samples(deps_.settings->tracks[t.ordinal].offset_us.load());
            const int64_t read_pos = mixer_->position_samples() - offset;
            const int64_t end = t.fifo.end_pos();
            if (t.serial == current && (t.eos.load() || (end != INT64_MIN && end - read_pos > ahead))) continue;

            QueuedPacket qp;
            if (!t.packets.try_pop(qp)) continue;
            worked = true;
            PacketPtr owned(qp.packet);
            if (qp.serial != serial_.load()) continue;
            if (qp.serial != t.serial) {
                avcodec_flush_buffers(t.decoder.get());
                t.converter.reset();
                t.fifo.clear();
                t.serial = qp.serial;
                t.eos.store(false);
                t.drop_before = mixer_->to_samples(gen_audio_start_us_.load()) - offset;
            }
            if (!owned) {
                decode_audio_packet(t, nullptr);
                int64_t pos = 0;
                if (const int n = t.converter.drain(t.convert, pos); n > 0) t.fifo.write(t.convert.data(), n, pos);
                t.eos.store(true);
                continue;
            }
            decode_audio_packet(t, owned.get());
        }
        if (!worked) WaitForMultipleObjects(2, waits, FALSE, 20);
    }
}

}  // namespace syna
