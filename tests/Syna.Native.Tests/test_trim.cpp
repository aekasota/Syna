#include "test.h"

#include "common/ffmpeg.h"
#include "common/util.h"
#include "syna_api.h"
#include "media/media_info.h"
#include "trim/h264_bitstream.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <thread>

using namespace syna;
using namespace std::chrono_literals;

// The managed mirrors (Native.TrimRequestNative, Native.TrimTrackNative) are checked against the same numbers.
static_assert(sizeof(syna_trim_request) == 112 && offsetof(syna_trim_request, video) == 40 && offsetof(syna_trim_request, audio) == 76 &&
              offsetof(syna_trim_request, audio_channels) == 92 && offsetof(syna_trim_request, tracks) == 96 &&
              offsetof(syna_trim_request, track_count) == 104);
static_assert(sizeof(syna_trim_track) == 16 && offsetof(syna_trim_track, gain) == 4 && offsetof(syna_trim_track, offset_us) == 8);

namespace {

struct JobResult {
    syna_job_status status{};
    std::string json;
};

JobResult wait_job(syna_job* job, std::chrono::seconds timeout = 120s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    JobResult r;
    for (;;) {
        syna_job_status_get(job, &r.status);
        if (r.status.state != SYNA_JOB_RUNNING) break;
        if (std::chrono::steady_clock::now() > deadline) throw syna_test::Failure("job timeout");
        std::this_thread::sleep_for(10ms);
    }
    char* json = nullptr;
    if (syna_job_result_json(job, &json) == SYNA_OK && json) {
        r.json = json;
        syna_free_string(json);
    }
    syna_job_destroy(job);
    return r;
}

std::wstring fresh_output(const char* name) {
    std::wstring p = syna_test::temp_path(name);
    std::filesystem::remove(p);
    return p;
}

// A trim with the default output settings (video copied as far as the mode allows, tracks mixed),
// or with `set` adjusting the request.
JobResult trim(const char* fixture, const std::wstring& out, int64_t in_us, int64_t out_us, int mode,
               const std::function<void(syna_trim_request&)>& set = {}) {
    const std::wstring src = syna_test::fixture(fixture);
    syna_trim_request req{};
    req.source = src.c_str();
    req.destination = out.c_str();
    req.in_us = in_us;
    req.out_us = out_us;
    req.mode = mode;
    req.allow_hw_encode = 1;
    if (set) set(req);
    syna_job* job = nullptr;
    CHECK(syna_trim_start(&req, &job) == SYNA_OK);
    return wait_job(job);
}

void separate(syna_trim_request& r) { r.audio = SYNA_TRIM_AUDIO_SEPARATE; }

// Mixer entries for a fixture's audio tracks, in track order: {gain, offset_us} each, keyed by the
// stream index the probe reports (as the app sends them).
std::vector<syna_trim_track> mixer_of(const char* fixture, const std::vector<std::pair<float, int64_t>>& tracks) {
    MediaInfo m;
    CHECK(probe_media(syna_test::fixture(fixture), true, m) == SYNA_OK);
    std::vector<syna_trim_track> out;
    for (size_t i = 0; i < tracks.size() && i < m.audio.size(); ++i) out.push_back({m.audio[i].stream_index, tracks[i].first, tracks[i].second});
    return out;
}

void use(syna_trim_request& r, const std::vector<syna_trim_track>& tracks) {
    r.tracks = tracks.data();
    r.track_count = static_cast<int32_t>(tracks.size());
}

double db(double level, double reference) { return 20 * std::log10(std::max(level, 1e-9) / reference); }

bool has(const std::string& json, const std::string& needle) { return json.find(needle) != std::string::npos; }

// Decodes every frame of the file's video stream; returns luma planes and pts (us, timeline).
// keep selects which frames' luma is stored (others get an empty plane, so indices still match).
struct Decoded {
    std::vector<std::vector<uint8_t>> luma;
    std::vector<int64_t> pts_us;
    int corrupt = 0;
};

Decoded decode_all(const std::wstring& path, size_t max_frames = 100000, bool keep_pixels = true,
                   const std::function<bool(size_t)>& keep = {}) {
    std::string err;
    FormatPtr fmt = open_input(path, true, &err);
    CHECK_MSG(fmt != nullptr, "open {}", err);
    const int vi = best_video_stream(fmt.get());
    AVStream* st = fmt->streams[vi];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    CHECK(avcodec_open2(ctx.get(), codec, nullptr) >= 0);
    const int64_t start = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
    Decoded d;
    PacketPtr pkt = make_packet();
    FramePtr frame = make_frame();
    auto take = [&] {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            if (frame->flags & AV_FRAME_FLAG_CORRUPT || frame->decode_error_flags) ++d.corrupt;
            if (d.pts_us.size() < max_frames) {
                d.pts_us.push_back(ts_to_us(frame->best_effort_timestamp, st->time_base) - start);
                if (keep_pixels && (!keep || keep(d.pts_us.size() - 1))) {
                    std::vector<uint8_t> y(static_cast<size_t>(frame->width) * frame->height);
                    for (int r = 0; r < frame->height; ++r) std::memcpy(y.data() + static_cast<size_t>(r) * frame->width, frame->data[0] + static_cast<ptrdiff_t>(r) * frame->linesize[0], frame->width);
                    d.luma.push_back(std::move(y));
                } else if (keep_pixels) {
                    d.luma.emplace_back();
                }
            }
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0 && d.pts_us.size() < max_frames) {
        if (pkt->stream_index == vi) avcodec_send_packet(ctx.get(), pkt.get());
        av_packet_unref(pkt.get());
        take();
    }
    avcodec_send_packet(ctx.get(), nullptr);
    take();
    return d;
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0;
    double mse = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double e = static_cast<double>(a[i]) - b[i];
        mse += e * e;
    }
    mse /= static_cast<double>(a.size());
    return mse == 0 ? 999.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

MediaInfo probe(const std::wstring& path) {
    MediaInfo m;
    CHECK(probe_media(path, true, m) == SYNA_OK);
    return m;
}

std::filesystem::file_time_type mtime(const std::wstring& p) { return std::filesystem::last_write_time(p); }

constexpr int64_t kF = 16667;  // 60 fps frame

// First channel of one audio stream (by order among the audio streams), decoded at its own rate,
// with the timeline time of its first sample.
struct Pcm {
    std::vector<float> x;
    int rate = 0;
    double start_s = 0;
    double duration_s() const { return rate ? static_cast<double>(x.size()) / rate : 0; }
};

Pcm decode_audio(const std::wstring& path, int ordinal = 0) {
    std::string err;
    FormatPtr fmt = open_input(path, true, &err);
    CHECK_MSG(fmt != nullptr, "open {}", err);
    int si = -1;
    for (unsigned i = 0, seen = 0; i < fmt->nb_streams && si < 0; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && static_cast<int>(seen++) == ordinal) si = static_cast<int>(i);
    CHECK(si >= 0);
    AVStream* st = fmt->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    CHECK(avcodec_open2(ctx.get(), codec, nullptr) >= 0);
    const int64_t start = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
    Pcm p;
    p.rate = ctx->sample_rate;
    SwrPtr swr;
    PacketPtr pkt = make_packet();
    FramePtr frame = make_frame();
    std::vector<float> planar;
    auto take = [&] {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            if (!swr) {
                SwrContext* s = nullptr;
                swr_alloc_set_opts2(&s, &frame->ch_layout, AV_SAMPLE_FMT_FLTP, frame->sample_rate, &frame->ch_layout,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
                CHECK(swr_init(s) >= 0);
                swr.reset(s);
                p.start_s = (ts_to_us(frame->best_effort_timestamp, st->time_base) - start) / 1e6;
            }
            const int ch = frame->ch_layout.nb_channels;
            planar.resize(static_cast<size_t>(frame->nb_samples) * ch);
            std::vector<uint8_t*> planes(static_cast<size_t>(ch));
            for (int c = 0; c < ch; ++c) planes[static_cast<size_t>(c)] = reinterpret_cast<uint8_t*>(planar.data() + static_cast<size_t>(c) * frame->nb_samples);
            const int n = swr_convert(swr.get(), planes.data(), frame->nb_samples, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            p.x.insert(p.x.end(), planar.begin(), planar.begin() + std::max(0, n));
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == si) avcodec_send_packet(ctx.get(), pkt.get());
        av_packet_unref(pkt.get());
        take();
    }
    avcodec_send_packet(ctx.get(), nullptr);
    take();
    return p;
}

// Amplitude of a sine at `freq` over the timeline window [t0, t1) (Goertzel).
double tone(const Pcm& p, double freq, double t0, double t1) {
    const int64_t a = std::clamp<int64_t>(std::llround((t0 - p.start_s) * p.rate), 0, static_cast<int64_t>(p.x.size()));
    const int64_t b = std::clamp<int64_t>(std::llround((t1 - p.start_s) * p.rate), a, static_cast<int64_t>(p.x.size()));
    if (b - a < 16) return 0;
    const double k = 2 * std::cos(2 * std::numbers::pi * freq / p.rate);
    double s1 = 0, s2 = 0;
    for (int64_t n = a; n < b; ++n) {
        const double s = p.x[static_cast<size_t>(n)] + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / static_cast<double>(b - a);
}

// Phase (radians) of the `freq` component over [t0, t1), against a reference whose phase is zero
// at timeline time -offset_s. Two signals carrying the same source sine agree when aligned.
double phase(const Pcm& p, double freq, double t0, double t1, double offset_s) {
    double i = 0, q = 0;
    const double w = 2 * std::numbers::pi * freq;
    for (int64_t n = std::llround((t0 - p.start_s) * p.rate); n < std::llround((t1 - p.start_s) * p.rate); ++n) {
        if (n < 0 || n >= static_cast<int64_t>(p.x.size())) continue;
        const double t = p.start_s + static_cast<double>(n) / p.rate + offset_s;
        i += p.x[static_cast<size_t>(n)] * std::sin(w * t);
        q += p.x[static_cast<size_t>(n)] * std::cos(w * t);
    }
    return std::atan2(q, i);
}

// Seconds between two phases of `freq`, wrapped to half a period.
double phase_delay(double a, double b, double freq) {
    double d = a - b;
    while (d > std::numbers::pi) d -= 2 * std::numbers::pi;
    while (d < -std::numbers::pi) d += 2 * std::numbers::pi;
    return d / (2 * std::numbers::pi * freq);
}

// First time in [from, to) where a 2 ms window holds `freq` above `level`, i.e. a tone onset.
double onset(const Pcm& p, double freq, double from, double to, double level) {
    for (double t = from; t < to; t += 0.00025)
        if (tone(p, freq, t, t + 0.002) > level) return t + 0.001;
    return -1;
}

// Discontinuities in a mix of two steady sines: the cascade of the two tones' recurrences
// (x[n+1] + x[n-1] - 2cos(w)x[n]) cancels both exactly, so what is left is codec noise, plus a
// spike wherever samples were dropped, repeated or replaced by silence.
int discontinuities2(const Pcm& p, double f1, double f2, double skip_s) {
    const double k1 = 2 * std::cos(2 * std::numbers::pi * f1 / p.rate), k2 = 2 * std::cos(2 * std::numbers::pi * f2 / p.rate);
    const size_t skip = static_cast<size_t>(skip_s * p.rate);
    double amp = 0;
    for (size_t n = skip; n < p.x.size(); ++n) amp = std::max(amp, static_cast<double>(std::fabs(p.x[n])));
    CHECK_MSG(amp > 0.01, "mix missing (peak {})", amp);
    auto y = [&](size_t n) { return static_cast<double>(p.x[n + 1]) + p.x[n - 1] - k1 * p.x[n]; };
    int events = 0;
    bool inside = false;
    for (size_t n = skip + 2; n + 2 < p.x.size() - static_cast<size_t>(p.rate / 10); ++n) {
        const bool bad = std::fabs(y(n + 1) + y(n - 1) - k2 * y(n)) > 0.02 * amp;
        if (bad && !inside) ++events;
        inside = bad;
    }
    return events;
}

// The single audio track that a trim of two or more tracks must produce.
void check_one_mixed_track(const JobResult& r, const MediaInfo& m, const char* codec, int rate) {
    CHECK_MSG(has(r.json, "\"audioMethod\":\"mix\""), "{}", r.json);
    CHECK_MSG(m.audio.size() == 1, "{} audio tracks", m.audio.size());
    CHECK_MSG(m.audio[0].codec == codec && m.audio[0].sample_rate == rate && m.audio[0].channels == 2, "{} {} Hz {} ch", m.audio[0].codec,
              m.audio[0].sample_rate, m.audio[0].channels);
}

int64_t json_int(const std::string& json, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = json.find(k);
    CHECK_MSG(at != std::string::npos, "{} missing", key);
    return std::stoll(json.substr(at + k.size()));
}

}  // namespace

TEST(trim_quick_copies_from_keyframe) {
    const std::wstring src = syna_test::fixture("A_h264_1080p60_2trk.mp4");
    const auto before = mtime(src);
    const std::wstring out = fresh_output("quick.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2510000, 5000000, SYNA_TRIM_QUICK);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK(has(r.json, "\"modeUsed\":\"quick\""));
    CHECK(has(r.json, "\"actualStartUs\":2500000"));
    CHECK(has(r.json, "\"passed\":true"));
    MediaInfo m = probe(out);
    CHECK(m.video.codec == "h264" && m.video.width == 1920);
    CHECK(m.video.frame_count == 150);
    check_one_mixed_track(r, m, "aac", 48000);
    CHECK(m.audio[0].title == "System sounds + Microphone");
    CHECK_NEAR(m.duration_us / 1e6, 2.5, 0.05);
    CHECK(mtime(src) == before);  // source untouched
    CHECK(!std::filesystem::exists(out + L".partial"));
}

TEST(trim_exact_is_frame_accurate_and_keeps_cadence) {
    const std::wstring out = fresh_output("exact.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2516667, 4000000, SYNA_TRIM_EXACT);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK(has(r.json, "\"modeUsed\":\"exact\""));
    CHECK(has(r.json, "\"hardwareEncoder\":true"));
    CHECK(has(r.json, "\"passed\":true"));
    MediaInfo m = probe(out);
    CHECK(m.video.frame_count == 89);
    CHECK_NEAR(m.video.fps_avg, 60.0, 0.1);
    check_one_mixed_track(r, m, "aac", 48000);
    Decoded src = decode_all(syna_test::fixture("A_h264_1080p60_2trk.mp4"), 160);
    Decoded dst = decode_all(out, 3);
    // The first output frame is source frame 151 (2.516667 s), not its neighbours.
    const double p151 = psnr(dst.luma[0], src.luma[151]);
    CHECK_MSG(p151 > 35.0, "psnr {}", p151);
    CHECK(p151 > psnr(dst.luma[0], src.luma[150]) + 3.0);
    CHECK(p151 > psnr(dst.luma[0], src.luma[152]) + 3.0);
    CHECK(dst.pts_us[0] == 0);
}

TEST(trim_smart_reencodes_only_the_partial_gop) {
    const std::wstring out = fresh_output("smart.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2516667, 6000000, SYNA_TRIM_SMART);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"modeUsed\":\"smart\""), "{}", r.json);
    CHECK(has(r.json, "\"videoMethod\":\"smart\""));
    CHECK(has(r.json, "\"passed\":true"));
    MediaInfo m = probe(out);
    CHECK(m.video.frame_count == 209);
    // Two parameter-set pairs in avcC: the source's and the renumbered encoder's.
    std::string err;
    FormatPtr f = open_input(out, true, &err);
    AVCodecParameters* par = f->streams[best_video_stream(f.get())]->codecpar;
    auto cfg = h264::parse_avcc(par->extradata, static_cast<size_t>(par->extradata_size));
    CHECK(cfg && cfg->sps.size() == 2 && cfg->pps.size() == 2);

    Decoded src = decode_all(syna_test::fixture("A_h264_1080p60_2trk.mp4"), 400);
    Decoded dst = decode_all(out);
    CHECK(dst.corrupt == 0);
    CHECK(dst.pts_us.size() == 209);
    CHECK(psnr(dst.luma[0], src.luma[151]) > 35.0);
    // From the splice (frame 180 = 3.0 s) on, frames are the source bitstream: identical pixels.
    CHECK(psnr(dst.luma[29], src.luma[180]) == 999.0);
    CHECK(psnr(dst.luma[208], src.luma[359]) == 999.0);
    // Timestamps continue across the splice without a gap.
    CHECK_NEAR(dst.pts_us[29] - dst.pts_us[28], kF, 2);
}

TEST(trim_smart_on_keyframe_is_pure_copy) {
    const std::wstring out = fresh_output("smart_copy.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 3000000, 4000000, SYNA_TRIM_SMART);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
    CHECK(has(r.json, "\"videoMethod\":\"copy\""));
    CHECK(has(r.json, "\"reencodedSeconds\":0"));
}

TEST(trim_smart_without_idr_in_selection_uses_exact) {
    // H has an IDR every 5 s; [7.3, 9.0) contains none, so there is nothing to copy.
    const std::wstring out = fresh_output("smart_no_idr.mp4");
    JobResult r = trim("H_h264_longgop_bframes.mp4", out, 7300000, 9000000, SYNA_TRIM_SMART);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
    CHECK(has(r.json, "\"modeUsed\":\"exact\""));
    CHECK(has(r.json, "no IDR keyframe inside the selection"));
    MediaInfo m = probe(out);
    CHECK_NEAR(m.video.frame_count, 102, 1);
}

TEST(trim_smart_splices_reordered_source_at_idr) {
    // H: x264 with 3 B-frames, closed GOPs, IDR at 10.0 s. 9.0-10.0 s is re-encoded, the rest copied.
    const std::wstring out = fresh_output("smart_bframes.mp4");
    JobResult r = trim("H_h264_longgop_bframes.mp4", out, 9000000, 14000000, SYNA_TRIM_SMART);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"modeUsed\":\"smart\""), "{}", r.json);
    CHECK(has(r.json, "\"videoMethod\":\"smart\""));
    const size_t at = r.json.find("\"reencodedSeconds\":");
    CHECK(at != std::string::npos);
    CHECK_NEAR(std::stod(r.json.substr(at + 19)), 1.0, 0.02);
    CHECK(has(r.json, "\"passed\":true"));

    auto wanted = [](size_t i) { return i == 540 || i == 600 || i == 839; };
    Decoded src = decode_all(syna_test::fixture("H_h264_longgop_bframes.mp4"), 840, true, wanted);
    Decoded dst = decode_all(out, 100000, true, [](size_t i) { return i == 0 || i == 60 || i == 299; });
    CHECK(dst.corrupt == 0);
    // The decode-order prefix may add at most a reordered frame or two displayed after OUT.
    CHECK_MSG(dst.pts_us.size() >= 300 && dst.pts_us.size() <= 303, "frames {}", dst.pts_us.size());
    CHECK(psnr(dst.luma[0], src.luma[540]) > 35.0);
    // From the IDR on, frames are the source bitstream: identical pixels.
    CHECK(psnr(dst.luma[60], src.luma[600]) == 999.0);
    CHECK(psnr(dst.luma[299], src.luma[839]) == 999.0);
    // Presentation is continuous across the splice and strictly increasing.
    CHECK_NEAR(dst.pts_us[60] - dst.pts_us[59], kF, 2);
    for (size_t i = 1; i < dst.pts_us.size(); ++i) CHECK(dst.pts_us[i] > dst.pts_us[i - 1]);
}

TEST(trim_webm_exact_keeps_a_webm_codec) {
    // WebM only carries VP8/VP9/AV1: the encoder choice must follow the container.
    const std::wstring out = fresh_output("exact.webm");
    JobResult r = trim("N_vp9_opus.webm", out, 1000000, 3000000, SYNA_TRIM_EXACT);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK(has(r.json, "\"passed\":true"));
    MediaInfo m = probe(out);
    CHECK_MSG(m.video.codec == "vp9" || m.video.codec == "av1", "codec {}", m.video.codec);
    CHECK(m.audio.size() == 1 && m.audio[0].codec == "opus");
    Decoded d = decode_all(out, 100000, false);  // Matroska/WebM store no frame count
    CHECK(d.corrupt == 0);
    CHECK_NEAR(d.pts_us.size(), 60, 1);
}

TEST(trim_exact_preserves_vfr_timestamps) {
    const std::wstring out = fresh_output("vfr.mp4");
    JobResult r = trim("I_h264_vfr.mp4", out, 1000000, 2000000, SYNA_TRIM_EXACT);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
    Decoded src = decode_all(syna_test::fixture("I_h264_vfr.mp4"), 200, false);
    Decoded dst = decode_all(out, 100000, false);
    const size_t at = r.json.find("\"actualStartUs\":");
    CHECK(at != std::string::npos);
    const int64_t start = std::stoll(r.json.substr(at + 16));
    size_t first = 0;
    while (first < src.pts_us.size() && src.pts_us[first] < start) ++first;
    CHECK(first < src.pts_us.size() && src.pts_us[first] == start);
    CHECK(dst.pts_us.size() > 50);
    for (size_t i = 1; i < dst.pts_us.size(); ++i) {
        const int64_t a = dst.pts_us[i] - dst.pts_us[i - 1];
        const int64_t b = src.pts_us[first + i] - src.pts_us[first + i - 1];
        CHECK_MSG(std::llabs(a - b) <= 1, "interval {} vs {} at {}", a, b, i);
    }
}

TEST(trim_hevc_10bit_exact_keeps_depth) {
    const std::wstring out = fresh_output("hevc10.mp4");
    JobResult r = trim("J_hevc_main10.mp4", out, 1000000, 2500000, SYNA_TRIM_SMART);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
    MediaInfo m = probe(out);
    MediaInfo s = probe(syna_test::fixture("J_hevc_main10.mp4"));
    CHECK(m.video.codec == "hevc");
    CHECK(m.video.bit_depth == 10);
    CHECK(m.video.color_transfer == s.video.color_transfer && m.video.color_primaries == s.video.color_primaries);
    CHECK(s.video.color_transfer == "bt709");
}

TEST(trim_hevc_exact_reencode_keeps_depth_and_color) {
    const std::wstring out = fresh_output("hevc10_exact.mp4");
    JobResult r = trim("J_hevc_main10.mp4", out, 1016667, 2500000, SYNA_TRIM_EXACT);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
    CHECK(has(r.json, "\"hardwareEncoder\":true"));
    MediaInfo m = probe(out);
    CHECK(m.video.codec == "hevc" && m.video.bit_depth == 10);
    CHECK(m.video.color_transfer == "bt709" && m.video.color_primaries == "bt709" && m.video.color_space == "bt709");
}

TEST(trim_matroska_quick) {
    const std::wstring out = fresh_output("quick.mkv");
    JobResult r = trim("M_h264_2trk.mkv", out, 1000000, 3000000, SYNA_TRIM_QUICK);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
    MediaInfo m = probe(out);
    CHECK(m.container == "Matroska");
    check_one_mixed_track(r, m, "aac", 48000);
    CHECK(m.audio[0].title == "System sounds + Microphone");
}

TEST(trim_mix_contains_both_tracks_in_sync) {
    // A: track 1 a steady 440 Hz tone (0.088 per channel), track 2 1 kHz beeps (0.283) during the
    // first half of every second. Every mode must output one track carrying both, lined up with
    // the source to well under a millisecond.
    const std::wstring a = syna_test::fixture("A_h264_1080p60_2trk.mp4");
    const Pcm src_tone = decode_audio(a, 0), src_beeps = decode_audio(a, 1);
    const double src_onset = onset(src_beeps, 1000, 2.9, 3.1, 0.14);
    CHECK_NEAR(src_onset, 3.0, 0.003);
    struct Case {
        int mode;
        int64_t in_us, out_us;
        const char* name;
    };
    for (const Case& c : {Case{SYNA_TRIM_QUICK, 2510000, 5000000, "mix_quick.mp4"}, Case{SYNA_TRIM_SMART, 2516667, 6000000, "mix_smart.mp4"},
                          Case{SYNA_TRIM_EXACT, 2516667, 5000000, "mix_exact.mp4"}}) {
        const std::wstring out = fresh_output(c.name);
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, c.in_us, c.out_us, c.mode);
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
        CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
        CHECK(has(r.json, "\"audioTracks\":2"));
        const MediaInfo m = probe(out);
        check_one_mixed_track(r, m, "aac", 48000);
        const double s = json_int(r.json, "actualStartUs") / 1e6;
        const double video_s = (json_int(r.json, "actualEndUs") - json_int(r.json, "actualStartUs")) / 1e6;
        const Pcm mix = decode_audio(out);
        CHECK_MSG(std::fabs(mix.start_s) < 0.0005, "mode {}: audio starts at {} s", c.mode, mix.start_s);
        CHECK_MSG(std::fabs(mix.duration_s() - video_s) < 0.03, "mode {}: audio {} s, video {} s", c.mode, mix.duration_s(), video_s);
        // Both sources at their recorded levels.
        const double on = std::ceil(s) - s;  // first beep starts here
        CHECK_NEAR(tone(mix, 440, 0.1, on - 0.05), 0.088, 0.01);
        CHECK_NEAR(tone(mix, 1000, on + 0.05, on + 0.45), 0.283, 0.03);
        CHECK(tone(mix, 1000, on + 0.55, on + 0.95) < 0.01);
        // Alignment with the source timeline: beep onset and tone phase.
        const double out_onset = onset(mix, 1000, on - 0.1, on + 0.1, 0.14);
        CHECK_MSG(std::fabs((out_onset + s) - src_onset) < 0.001, "mode {}: beep at {} s, source {} s", c.mode, out_onset + s, src_onset);
        const double lag = phase_delay(phase(mix, 440, 0.2, 1.2, s), phase(src_tone, 440, s + 0.2, s + 1.2, 0), 440);
        CHECK_MSG(std::fabs(lag) < 0.0002, "mode {}: 440 Hz tone off by {} ms", c.mode, lag * 1000);
    }
}

TEST(trim_mix_keeps_a_late_track_late) {
    // F: the beep track starts 0.5 s after the video and the tone.
    const std::wstring out = fresh_output("mix_late.mp4");
    JobResult r = trim("F_h264_offset_track.mp4", out, 0, 3000000, SYNA_TRIM_EXACT);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
    const MediaInfo m = probe(out);
    check_one_mixed_track(r, m, "aac", 48000);
    const Pcm mix = decode_audio(out);
    CHECK(tone(mix, 440, 0.05, 0.45) > 0.07);
    CHECK(tone(mix, 1000, 0.05, 0.45) < 0.005);  // silence before the late track
    CHECK_NEAR(onset(mix, 1000, 0.3, 0.7, 0.14), 0.5, 0.003);
}

TEST(trim_mix_resamples_mixed_rates_continuously) {
    // E: a 48 kHz 440 Hz track and a 44.1 kHz 1 kHz track become one 48 kHz track.
    const std::wstring out = fresh_output("mix_rates.mp4");
    JobResult r = trim("E_h264_mixed_rates.mp4", out, 1000000, 4000000, SYNA_TRIM_SMART);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
    const MediaInfo m = probe(out);
    check_one_mixed_track(r, m, "aac", 48000);
    const Pcm mix = decode_audio(out);
    CHECK_NEAR(tone(mix, 440, 0.2, 2.8), 0.088, 0.01);
    CHECK_NEAR(tone(mix, 1000, 0.2, 2.8), 0.088, 0.01);
    const int bad = discontinuities2(mix, 440, 1000, 0.05);
    CHECK_MSG(bad == 0, "{} discontinuities in the mix", bad);
}

TEST(trim_mix_takes_every_track) {
    // D: 440 Hz, 1 kHz beeps, 220 Hz and pink noise; all four end up in the one track.
    const std::wstring out = fresh_output("mix_four.mp4");
    JobResult r = trim("D_h264_4trk.mp4", out, 1000000, 4000000, SYNA_TRIM_QUICK);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK(has(r.json, "\"audioTracks\":4"));
    const MediaInfo m = probe(out);
    check_one_mixed_track(r, m, "aac", 48000);
    const Pcm mix = decode_audio(out);
    CHECK_NEAR(tone(mix, 440, 1.05, 1.45), 0.088, 0.015);
    CHECK_NEAR(tone(mix, 220, 1.05, 1.45), 0.088, 0.015);
    CHECK_NEAR(tone(mix, 1000, 1.05, 1.45), 0.283, 0.03);
    CHECK(tone(mix, 1000, 1.55, 1.95) < 0.02);
}

TEST(trim_audio_only_mix_is_continuous) {
    // O: two audio tracks with ShadowPlay-like timestamp jitter and drift, no video. The export
    // must place them exactly like playback: continuous, with both tones.
    const std::wstring out = fresh_output("mix_jitter.m4a");
    JobResult r = trim("O_aac_2trk_jitter.mp4", out, 5000000, 15000000, SYNA_TRIM_EXACT);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
    const MediaInfo m = probe(out);
    check_one_mixed_track(r, m, "aac", 48000);
    const Pcm mix = decode_audio(out);
    CHECK_NEAR(mix.duration_s(), 10.0, 0.03);
    // Track 2 is stretched by up to 0.2 % to follow its drifting timestamps (1 kHz becomes up to
    // 1002 Hz), so its level is measured over windows short enough not to notice.
    for (double t : {1.0, 5.0, 9.0}) {
        CHECK_NEAR(tone(mix, 440, t, t + 0.1), 0.088, 0.01);
        CHECK_NEAR(tone(mix, 1000, t, t + 0.1), 0.088, 0.01);
    }
    const int bad = discontinuities2(mix, 440, 1000, 0.05);
    CHECK_MSG(bad == 0, "{} discontinuities in the mix", bad);
    // The detector does see a single missing sample in this signal.
    Pcm cut = mix;
    cut.x.erase(cut.x.begin() + cut.rate * 5);
    CHECK(discontinuities2(cut, 440, 1000, 0.05) == 1);
}

TEST(trim_mix_handles_a_mono_track_that_ends_early) {
    // Q (Matroska): an AAC stereo 440 Hz tone for 3 s and a mono Opus 1 kHz tone that stops at 2 s.
    const std::wstring out = fresh_output("mix_mono.mkv");
    JobResult r = trim("Q_h264_aac_opus_mono.mkv", out, 0, 3000000, SYNA_TRIM_QUICK);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
    const MediaInfo m = probe(out);
    check_one_mixed_track(r, m, "aac", 48000);
    const Pcm mix = decode_audio(out);
    CHECK_NEAR(tone(mix, 440, 0.1, 2.9), 0.088, 0.01);
    CHECK_NEAR(tone(mix, 1000, 0.1, 1.9), 0.088, 0.01);  // mono in both channels at -3 dB
    CHECK(tone(mix, 1000, 2.1, 2.9) < 0.005);              // silence after the short track ends
    CHECK_NEAR(mix.duration_s(), 3.0, 0.05);
}

TEST(trim_mix_to_webm_uses_opus) {
    const std::wstring out = fresh_output("mix.webm");
    JobResult r = trim("P_vp9_2trk.webm", out, 1000000, 3000000, SYNA_TRIM_EXACT);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
    const MediaInfo m = probe(out);
    check_one_mixed_track(r, m, "opus", 48000);
    const Pcm mix = decode_audio(out);
    CHECK_NEAR(tone(mix, 440, 0.1, 0.45), 0.088, 0.015);
    CHECK_NEAR(tone(mix, 1000, 0.05, 0.45), 0.283, 0.04);
}

TEST(trim_single_track_is_stream_copied) {
    // C has one audio track: it is copied, not re-encoded.
    const std::wstring src = syna_test::fixture("C_h264_720p30_1trk.mp4");
    const std::wstring out = fresh_output("single.mp4");
    JobResult r = trim("C_h264_720p30_1trk.mp4", out, 2000000, 4000000, SYNA_TRIM_QUICK);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"audioMethod\":\"copy\""), "{}", r.json);
    const MediaInfo m = probe(out), s = probe(src);
    CHECK(m.audio.size() == 1 && m.audio[0].codec == s.audio[0].codec && m.audio[0].bit_rate > 0);
    // The copied packets are the source's packets, byte for byte.
    auto audio_packets = [](const std::wstring& path, int64_t from_us, int count) {
        std::string err;
        FormatPtr f = open_input(path, true, &err);
        int si = -1;
        for (unsigned i = 0; i < f->nb_streams; ++i)
            if (f->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) si = static_cast<int>(i);
        std::vector<std::vector<uint8_t>> out;
        PacketPtr pkt = make_packet();
        while (static_cast<int>(out.size()) < count && av_read_frame(f.get(), pkt.get()) >= 0) {
            if (pkt->stream_index == si && ts_to_us(pkt->pts, f->streams[si]->time_base) >= from_us) out.emplace_back(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt.get());
        }
        return out;
    };
    const auto a = audio_packets(out, 0, 20);
    const auto b = audio_packets(src, json_int(r.json, "actualStartUs"), 20);
    CHECK(a.size() == 20 && a == b);
}

TEST(trim_separate_keeps_each_track_on_its_own_in_sync) {
    // A: track 1 a steady 440 Hz tone, track 2 1 kHz beeps. Kept separate, each output track holds
    // only its own source track, copied packet for packet, on the source timeline.
    const std::wstring a = syna_test::fixture("A_h264_1080p60_2trk.mp4");
    const Pcm src_tone = decode_audio(a, 0);
    const double src_onset = onset(decode_audio(a, 1), 1000, 2.9, 3.1, 0.14);
    struct Case {
        int mode;
        int64_t in_us, out_us;
        const char* name;
    };
    for (const Case& c : {Case{SYNA_TRIM_QUICK, 2510000, 5000000, "sep_quick.mp4"}, Case{SYNA_TRIM_SMART, 2516667, 6000000, "sep_smart.mp4"},
                          Case{SYNA_TRIM_EXACT, 2516667, 5000000, "sep_exact.mp4"}}) {
        const std::wstring out = fresh_output(c.name);
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, c.in_us, c.out_us, c.mode, separate);
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
        CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
        CHECK_MSG(has(r.json, "\"audioMethod\":\"copy\""), "{}", r.json);
        const MediaInfo m = probe(out);
        CHECK_MSG(m.audio.size() == 2, "mode {}: {} audio tracks", c.mode, m.audio.size());
        CHECK(m.audio[0].codec == "aac" && m.audio[1].codec == "aac" && m.audio[0].title == "System sounds" && m.audio[1].title == "Microphone");
        const double s = json_int(r.json, "actualStartUs") / 1e6;
        const double video_s = (json_int(r.json, "actualEndUs") - json_int(r.json, "actualStartUs")) / 1e6;
        const double on = std::ceil(s) - s;
        const Pcm t1 = decode_audio(out, 0), t2 = decode_audio(out, 1);
        CHECK_MSG(std::fabs(t1.duration_s() + t1.start_s - video_s) < 0.03, "mode {}: track 1 ends at {} s, video {} s", c.mode, t1.duration_s() + t1.start_s, video_s);
        CHECK_NEAR(tone(t1, 440, 0.1, on - 0.05), 0.088, 0.01);
        CHECK(tone(t1, 1000, on + 0.05, on + 0.45) < 0.005);  // the beeps are not in track 1
        CHECK_NEAR(tone(t2, 1000, on + 0.05, on + 0.45), 0.283, 0.03);
        CHECK(tone(t2, 440, on + 0.55, on + 0.95) < 0.005);   // nor the tone in track 2
        const double out_onset = onset(t2, 1000, on - 0.1, on + 0.1, 0.14);
        CHECK_MSG(std::fabs((out_onset + s) - src_onset) < 0.001, "mode {}: beep at {} s, source {} s", c.mode, out_onset + s, src_onset);
        // Copied tracks keep their packets' times; the edit list that places them is written at
        // 1/90000 s, so they line up with the source as closely as encoded ones.
        const double lag = phase_delay(phase(t1, 440, 0.2, 1.2, s), phase(src_tone, 440, s + 0.2, s + 1.2, 0), 440);
        CHECK_MSG(std::fabs(lag) < 0.0002, "mode {}: 440 Hz tone off by {} ms", c.mode, lag * 1000);
    }
}

TEST(trim_separate_reencodes_each_track_when_a_codec_is_requested) {
    const std::wstring out = fresh_output("sep_aac128.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2516667, 5000000, SYNA_TRIM_EXACT, [](syna_trim_request& q) {
        q.audio = SYNA_TRIM_AUDIO_SEPARATE;
        q.audio_codec = SYNA_AUDIO_CODEC_AAC;
        q.audio_bitrate_kbps = 128;
    });
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{} {}", r.status.error, r.json);
    CHECK_MSG(has(r.json, "\"passed\":true"), "{}", r.json);
    CHECK_MSG(has(r.json, "\"audioMethod\":\"encode\""), "{}", r.json);
    CHECK_MSG(has(r.json, "\"method\":\"encode\",\"tracks\":1,\"description\":\"AAC · 48 kHz · stereo · 128 kb/s\""), "{}", r.json);
    const MediaInfo m = probe(out);
    CHECK(m.audio.size() == 2);
    // AAC is encoded at an average rate: pure test tones need well below the 128 kb/s it may use.
    for (const AudioTrackInfo& t : m.audio) CHECK_MSG(t.codec == "aac" && t.bit_rate > 16000 && t.bit_rate <= 140000, "{} {} b/s", t.codec, t.bit_rate);
    const double s = json_int(r.json, "actualStartUs") / 1e6, on = std::ceil(s) - s;
    const Pcm t1 = decode_audio(out, 0), t2 = decode_audio(out, 1);
    CHECK(std::fabs(t1.start_s) < 0.0005 && std::fabs(t2.start_s) < 0.0005);  // encoded tracks start at the first video frame
    CHECK_NEAR(tone(t1, 440, 0.1, on - 0.05), 0.088, 0.01);
    CHECK(tone(t1, 1000, on + 0.05, on + 0.45) < 0.005);
    CHECK_NEAR(tone(t2, 1000, on + 0.05, on + 0.45), 0.283, 0.03);
    const double src_onset = onset(decode_audio(syna_test::fixture("A_h264_1080p60_2trk.mp4"), 1), 1000, 2.9, 3.1, 0.14);
    CHECK_NEAR(onset(t2, 1000, on - 0.1, on + 0.1, 0.14) + s, src_onset, 0.001);
}

TEST(trim_separate_keeps_rates_late_tracks_and_every_track) {
    {  // E: 48 kHz and 44.1 kHz tracks stay at their rates.
        const std::wstring out = fresh_output("sep_rates.mp4");
        JobResult r = trim("E_h264_mixed_rates.mp4", out, 1000000, 4000000, SYNA_TRIM_SMART, separate);
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{}", r.json);
        const MediaInfo m = probe(out);
        CHECK(m.audio.size() == 2 && m.audio[0].sample_rate == 48000 && m.audio[1].sample_rate == 44100);
    }
    {  // F: the beep track starts 0.5 s after the video and stays there.
        const std::wstring out = fresh_output("sep_late.mp4");
        JobResult r = trim("F_h264_offset_track.mp4", out, 0, 3000000, SYNA_TRIM_EXACT, separate);
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{}", r.json);
        const Pcm late = decode_audio(out, 1);
        CHECK_NEAR(onset(late, 1000, 0.3, 0.7, 0.14), 0.5, 0.003);
        CHECK(tone(decode_audio(out, 0), 440, 0.05, 0.45) > 0.07);
    }
    {  // D: four tracks stay four tracks, each with its own content.
        const std::wstring out = fresh_output("sep_four.mp4");
        JobResult r = trim("D_h264_4trk.mp4", out, 1000000, 4000000, SYNA_TRIM_QUICK, separate);
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{}", r.json);
        const MediaInfo m = probe(out);
        CHECK_MSG(m.audio.size() == 4, "{} audio tracks", m.audio.size());
        CHECK_NEAR(tone(decode_audio(out, 0), 440, 1.05, 1.45), 0.088, 0.015);
        CHECK_NEAR(tone(decode_audio(out, 1), 1000, 1.05, 1.45), 0.283, 0.03);
        CHECK_NEAR(tone(decode_audio(out, 2), 220, 1.05, 1.45), 0.088, 0.015);
        const Pcm noise = decode_audio(out, 3);
        double energy = 0;
        for (float v : noise.x) energy += static_cast<double>(v) * v;
        CHECK(noise.x.size() > 1000 && std::sqrt(energy / static_cast<double>(noise.x.size())) > 0.005);
        CHECK(tone(decode_audio(out, 2), 440, 1.05, 1.45) < 0.01);
    }
}

TEST(trim_mix_follows_the_requested_audio_settings) {
    {  // Lossless: FLAC in Matroska.
        const std::wstring out = fresh_output("mix_flac.mkv");
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2510000, 5000000, SYNA_TRIM_QUICK, [](syna_trim_request& q) { q.audio_codec = SYNA_AUDIO_CODEC_FLAC; });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        CHECK(has(r.json, "lossless, 24-bit"));
        const MediaInfo m = probe(out);
        CHECK(m.audio.size() == 1 && m.audio[0].codec == "flac" && m.audio[0].sample_rate == 48000 && m.audio[0].channels == 2);
        const Pcm mix = decode_audio(out);
        CHECK_NEAR(tone(mix, 440, 0.1, 0.45), 0.088, 0.01);
        CHECK_NEAR(tone(mix, 1000, 0.55, 0.95), 0.283, 0.03);
    }
    {  // AAC at 44.1 kHz, mono.
        const std::wstring out = fresh_output("mix_mono44.mp4");
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2510000, 5000000, SYNA_TRIM_QUICK, [](syna_trim_request& q) {
            q.audio_codec = SYNA_AUDIO_CODEC_AAC;
            q.audio_bitrate_kbps = 96;
            q.audio_sample_rate = 44100;
            q.audio_channels = 1;
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        const MediaInfo m = probe(out);
        CHECK(m.audio.size() == 1 && m.audio[0].codec == "aac" && m.audio[0].sample_rate == 44100 && m.audio[0].channels == 1);
        const Pcm mix = decode_audio(out);
        CHECK(tone(mix, 440, 0.1, 0.45) > 0.04 && tone(mix, 1000, 0.55, 0.95) > 0.12);
    }
    {  // Opus in MP4.
        const std::wstring out = fresh_output("mix_opus.mp4");
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2510000, 5000000, SYNA_TRIM_QUICK, [](syna_trim_request& q) { q.audio_codec = SYNA_AUDIO_CODEC_OPUS; });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        CHECK(probe(out).audio[0].codec == "opus");
    }
}

TEST(trim_mix_applies_the_mixer_in_every_mode) {
    // A: the tone (0.088) at half level, the beeps muted. The one mixed track carries the tone 6 dB
    // down and no beeps, still lined up with the source; the video path is the mode's own.
    const char* fx = "A_h264_1080p60_2trk.mp4";
    const auto tracks = mixer_of(fx, {{0.5f, 0}, {0.0f, 0}});
    const Pcm src_tone = decode_audio(syna_test::fixture(fx), 0);
    struct Case {
        int mode;
        int64_t in_us, out_us;
        const char* name;
        const char* video;
    };
    for (const Case& c : {Case{SYNA_TRIM_QUICK, 2510000, 5000000, "mixer_quick.mp4", "copy"}, Case{SYNA_TRIM_SMART, 2516667, 6000000, "mixer_smart.mp4", "smart"},
                          Case{SYNA_TRIM_EXACT, 2516667, 5000000, "mixer_exact.mp4", "reencode"}}) {
        const std::wstring out = fresh_output(c.name);
        JobResult r = trim(fx, out, c.in_us, c.out_us, c.mode, [&](syna_trim_request& q) { use(q, tracks); });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        CHECK_MSG(has(r.json, std::format("\"videoMethod\":\"{}\"", c.video)), "mode {}: {}", c.mode, r.json);
        CHECK_MSG(has(r.json, "\"method\":\"mix\",\"tracks\":2,") && has(r.json, "\"adjusted\":true"), "{}", r.json);
        check_one_mixed_track(r, probe(out), "aac", 48000);
        const double s = json_int(r.json, "actualStartUs") / 1e6, on = std::ceil(s) - s;
        const Pcm mix = decode_audio(out);
        const double level = tone(mix, 440, 0.1, on - 0.05);
        CHECK_MSG(std::fabs(db(level, 0.088) + 6.02) < 0.5, "mode {}: tone {} ({:.2f} dB)", c.mode, level, db(level, 0.088));
        CHECK_MSG(tone(mix, 1000, on + 0.05, on + 0.45) < 0.003, "mode {}: muted beeps at {}", c.mode, tone(mix, 1000, on + 0.05, on + 0.45));
        const double lag = phase_delay(phase(mix, 440, 0.2, 1.2, s), phase(src_tone, 440, s + 0.2, s + 1.2, 0), 440);
        CHECK_MSG(std::fabs(lag) < 0.0002, "mode {}: 440 Hz tone off by {} ms", c.mode, lag * 1000);
    }
    {  // Audio only (O): the second track muted leaves only the 440 Hz tone, at its level.
        const std::wstring out = fresh_output("mixer_audio_only.m4a");
        const auto muted = mixer_of("O_aac_2trk_jitter.mp4", {{1.0f, 0}, {0.0f, 0}});
        JobResult r = trim("O_aac_2trk_jitter.mp4", out, 5000000, 8000000, SYNA_TRIM_EXACT, [&](syna_trim_request& q) { use(q, muted); });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        const Pcm mix = decode_audio(out);
        CHECK_NEAR(tone(mix, 440, 1.0, 1.1), 0.088, 0.01);
        CHECK(tone(mix, 1000, 1.0, 1.1) < 0.003);
    }
}

TEST(trim_separate_applies_the_mixer_to_each_track) {
    // A, tracks kept separate. The muted Microphone (beeps) stays a track of its own, silent, for the
    // whole cut; System sounds, untouched by the mixer, is still copied packet for packet.
    const char* fx = "A_h264_1080p60_2trk.mp4";
    const auto muted = mixer_of(fx, {{1.0f, 0}, {0.0f, 0}});
    struct Case {
        int mode;
        int64_t in_us, out_us;
        const char* name;
    };
    for (const Case& c : {Case{SYNA_TRIM_QUICK, 2510000, 5000000, "mixer_sep_quick.mp4"}, Case{SYNA_TRIM_SMART, 2516667, 6000000, "mixer_sep_smart.mp4"},
                          Case{SYNA_TRIM_EXACT, 2516667, 5000000, "mixer_sep_exact.mp4"}}) {
        const std::wstring out = fresh_output(c.name);
        JobResult r = trim(fx, out, c.in_us, c.out_us, c.mode, [&](syna_trim_request& q) {
            separate(q);
            use(q, muted);
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        CHECK_MSG(has(r.json, "\"audioMethod\":\"copy+encode\""), "mode {}: {}", c.mode, r.json);
        CHECK_MSG(has(r.json, "\"method\":\"copy\",\"tracks\":1,") && has(r.json, "\"adjusted\":false") && has(r.json, "\"adjusted\":true"), "{}", r.json);
        const MediaInfo m = probe(out);
        CHECK_MSG(m.audio.size() == 2, "mode {}: {} audio tracks", c.mode, m.audio.size());
        CHECK(m.audio[0].title == "System sounds" && m.audio[1].title == "Microphone");
        const double s = json_int(r.json, "actualStartUs") / 1e6, on = std::ceil(s) - s;
        const double video_s = (json_int(r.json, "actualEndUs") - json_int(r.json, "actualStartUs")) / 1e6;
        const Pcm t1 = decode_audio(out, 0), t2 = decode_audio(out, 1);
        CHECK_NEAR(tone(t1, 440, 0.1, on - 0.05), 0.088, 0.01);
        CHECK_MSG(tone(t2, 1000, on + 0.05, on + 0.45) < 0.003, "mode {}: muted track carries {}", c.mode, tone(t2, 1000, on + 0.05, on + 0.45));
        CHECK_MSG(std::fabs(t2.start_s) < 0.0005 && std::fabs(t2.duration_s() - video_s) < 0.03, "mode {}: muted track {}..{} s, video {} s", c.mode,
                  t2.start_s, t2.start_s + t2.duration_s(), video_s);
    }
    {  // System sounds at half level (-6 dB); the Microphone, left alone, is copied.
        const std::wstring out = fresh_output("mixer_sep_half.mp4");
        const auto half = mixer_of(fx, {{0.5f, 0}, {1.0f, 0}});
        JobResult r = trim(fx, out, 2516667, 5000000, SYNA_TRIM_EXACT, [&](syna_trim_request& q) {
            separate(q);
            use(q, half);
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        CHECK_MSG(has(r.json, "{\"method\":\"encode\",\"tracks\":1,") && has(r.json, "{\"method\":\"copy\",\"tracks\":1,"), "{}", r.json);
        const double s = json_int(r.json, "actualStartUs") / 1e6, on = std::ceil(s) - s;
        const double level = tone(decode_audio(out, 0), 440, 0.1, on - 0.05);
        CHECK_MSG(std::fabs(db(level, 0.088) + 6.02) < 0.5, "tone {} ({:.2f} dB)", level, db(level, 0.088));
        CHECK_NEAR(tone(decode_audio(out, 1), 1000, on + 0.05, on + 0.45), 0.283, 0.03);
    }
    {  // The beeps boosted to 200 % (+6 dB) stay below full scale, so the limiter leaves them alone.
        const std::wstring out = fresh_output("mixer_sep_boost.mp4");
        const auto boost = mixer_of(fx, {{1.0f, 0}, {2.0f, 0}});
        JobResult r = trim(fx, out, 2516667, 5000000, SYNA_TRIM_EXACT, [&](syna_trim_request& q) {
            separate(q);
            use(q, boost);
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        const double s = json_int(r.json, "actualStartUs") / 1e6, on = std::ceil(s) - s;
        const double level = tone(decode_audio(out, 1), 1000, on + 0.05, on + 0.45);
        CHECK_MSG(std::fabs(db(level, 0.283) - 6.02) < 0.5, "beeps {} ({:.2f} dB)", level, db(level, 0.283));
    }
    {  // Entries at gain 1 without an offset change nothing: both tracks are still copied.
        const std::wstring out = fresh_output("mixer_sep_neutral.mp4");
        const auto neutral = mixer_of(fx, {{1.0f, 0}, {1.0f, 0}});
        JobResult r = trim(fx, out, 2510000, 5000000, SYNA_TRIM_QUICK, [&](syna_trim_request& q) {
            separate(q);
            use(q, neutral);
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"audioMethod\":\"copy\"") && !has(r.json, "\"adjusted\":true"), "{}", r.json);
    }
}

TEST(trim_mixer_offsets_move_tracks_like_playback) {
    // A: the beep onset at 3.0 s in the source. Delaying the beep track by 300 ms (or advancing it)
    // moves it by exactly that much in the output, as the player's offset does.
    const char* fx = "A_h264_1080p60_2trk.mp4";
    const double src_onset = onset(decode_audio(syna_test::fixture(fx), 1), 1000, 2.9, 3.1, 0.14);
    for (int64_t offset : {300000, -300000}) {
        for (int audio : {SYNA_TRIM_AUDIO_MIX, SYNA_TRIM_AUDIO_SEPARATE}) {
            const std::wstring out = fresh_output(offset > 0 ? "mixer_delay.mp4" : "mixer_advance.mp4");
            const auto moved = mixer_of(fx, {{1.0f, 0}, {1.0f, offset}});
            JobResult r = trim(fx, out, 2516667, 5000000, SYNA_TRIM_EXACT, [&](syna_trim_request& q) {
                q.audio = audio;
                use(q, moved);
            });
            CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
            const double s = json_int(r.json, "actualStartUs") / 1e6;
            const Pcm beeps = decode_audio(out, audio == SYNA_TRIM_AUDIO_MIX ? 0 : 1);
            const double expected = src_onset - s + offset / 1e6;
            const double got = onset(beeps, 1000, expected - 0.1, expected + 0.1, 0.14);
            CHECK_MSG(std::fabs(got - expected) < 0.001, "offset {} ms, audio {}: beep at {} s, expected {} s", offset / 1000, audio, got, expected);
            CHECK_NEAR(tone(decode_audio(out, 0), 440, 0.1, 0.4), 0.088, 0.01);  // the tone track is not moved
        }
    }
    {  // Beeps 2.5 s late and the tone 1 s early: audio comes from 2.5 s before IN and 1 s past OUT,
       // and the two tracks reach the mix 3.5 s apart; both stay complete.
        const std::wstring out = fresh_output("mixer_far.mp4");
        const auto spread = mixer_of(fx, {{1.0f, -1000000}, {1.0f, 2500000}});
        JobResult r = trim(fx, out, 2516667, 5000000, SYNA_TRIM_EXACT, [&](syna_trim_request& q) { use(q, spread); });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        const Pcm mix = decode_audio(out);
        const double s = json_int(r.json, "actualStartUs") / 1e6;
        const double length = (json_int(r.json, "actualEndUs") - json_int(r.json, "actualStartUs")) / 1e6;
        for (double t = 0.05; t + 0.1 < length - 0.02; t += 0.2) CHECK_MSG(std::fabs(tone(mix, 440, t, t + 0.1) - 0.088) < 0.01, "tone at {} s: {}", t, tone(mix, 440, t, t + 0.1));
        // Source second 1's beep (1.0-1.5 s) plays at 1.0 - s + 2.5 in the output.
        const double beep = 1.0 - s + 2.5;
        CHECK_NEAR(tone(mix, 1000, beep + 0.05, beep + 0.45), 0.283, 0.03);
        CHECK(tone(mix, 1000, beep - 0.4, beep - 0.05) < 0.005);
    }
}

TEST(trim_rejects_malformed_mixer_entries) {
    const std::wstring src = syna_test::fixture("A_h264_1080p60_2trk.mp4"), out = fresh_output("mixer_bad.mp4");
    syna_trim_request req{};
    req.source = src.c_str();
    req.destination = out.c_str();
    req.in_us = 1000000;
    req.out_us = 2000000;
    req.track_count = 1;  // and no array
    syna_job* job = nullptr;
    CHECK(syna_trim_start(&req, &job) == SYNA_E_INVALID_ARG && job == nullptr);
    const syna_trim_track nan_gain{1, std::nanf(""), 0};
    req.tracks = &nan_gain;
    CHECK(syna_trim_start(&req, &job) == SYNA_E_INVALID_ARG && job == nullptr);
}

TEST(trim_original_quick_cut_is_the_source_bitstream) {
    const std::wstring out = fresh_output("orig_quick.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2510000, 5000000, SYNA_TRIM_QUICK);
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
    CHECK(has(r.json, "\"videoMethod\":\"copy\"") && has(r.json, "\"videoHandling\":\"original\""));
    auto wanted = [](size_t i) { return i == 150 || i == 200 || i == 299; };
    Decoded src = decode_all(syna_test::fixture("A_h264_1080p60_2trk.mp4"), 300, true, wanted);
    Decoded dst = decode_all(out, 150, true, [](size_t i) { return i == 0 || i == 50 || i == 149; });
    CHECK(psnr(dst.luma[0], src.luma[150]) == 999.0 && psnr(dst.luma[50], src.luma[200]) == 999.0 && psnr(dst.luma[149], src.luma[299]) == 999.0);
}

TEST(trim_reencode_settings_apply_in_every_mode) {
    // A re-encode setting leaves nothing to copy: Quick Cut re-encodes and cuts frame-accurately.
    const std::wstring out = fresh_output("enc_quick.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2510000, 5000000, SYNA_TRIM_QUICK, [](syna_trim_request& q) {
        q.video = SYNA_TRIM_VIDEO_ENCODE;
        q.video_quality = 28;
    });
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
    CHECK(has(r.json, "\"modeUsed\":\"exact\"") && has(r.json, "\"videoMethod\":\"reencode\"") && has(r.json, "frame-accurately"));
    CHECK(has(r.json, "\"actualStartUs\":2500000"));
    const MediaInfo m = probe(out), s = probe(syna_test::fixture("A_h264_1080p60_2trk.mp4"));
    CHECK(m.video.codec == "h264" && m.video.width == 1920 && m.video.height == 1080);
    CHECK(m.video.frame_count == 150);
    CHECK_NEAR(m.video.fps_avg, 60.0, 0.1);  // never a different rate unless asked
    CHECK_MSG(m.video.bit_rate > 0 && m.video.bit_rate < s.video.bit_rate, "{} vs source {}", m.video.bit_rate, s.video.bit_rate);
    check_one_mixed_track(r, m, "aac", 48000);
}

TEST(trim_scales_down_and_converts_the_frame_rate_when_asked) {
    const std::wstring out = fresh_output("enc_720p30.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2500000, 5000000, SYNA_TRIM_EXACT, [](syna_trim_request& q) {
        q.video = SYNA_TRIM_VIDEO_ENCODE;
        q.video_height = 720;
        q.video_fps_num = 30;
        q.video_fps_den = 1;
    });
    CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
    const MediaInfo m = probe(out);
    CHECK(m.video.width == 1280 && m.video.height == 720);
    CHECK_NEAR(m.video.fps_avg, 30.0, 0.05);
    CHECK(m.video.frame_count == 75);
    Decoded d = decode_all(out, 100000, false);
    CHECK(d.corrupt == 0 && d.pts_us.size() == 75);
    for (size_t i = 1; i < d.pts_us.size(); ++i) CHECK_NEAR(d.pts_us[i] - d.pts_us[i - 1], 33333, 1);
    const Pcm mix = decode_audio(out);
    CHECK(std::fabs(mix.start_s) < 0.0005 && std::fabs(mix.duration_s() - 2.5) < 0.03);

    // Larger sizes and higher rates than the source are ignored rather than padded out.
    const std::wstring big = fresh_output("enc_big.mp4");
    JobResult b = trim("A_h264_1080p60_2trk.mp4", big, 2500000, 3500000, SYNA_TRIM_EXACT, [](syna_trim_request& q) {
        q.video = SYNA_TRIM_VIDEO_ENCODE;
        q.video_height = 2160;
        q.video_fps_num = 120;
        q.video_fps_den = 1;
    });
    CHECK_MSG(b.status.state == SYNA_JOB_SUCCEEDED, "{}", b.json);
    const MediaInfo mb = probe(big);
    CHECK(mb.video.width == 1920 && mb.video.height == 1080);
    CHECK_NEAR(mb.video.fps_avg, 60.0, 0.1);
}

TEST(trim_quality_and_bitrate_settings_control_the_size) {
    auto video_size = [](int quality, int kbps, int max_kbps, const char* name) {
        const std::wstring out = fresh_output(name);
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2500000, 5000000, SYNA_TRIM_EXACT, [&](syna_trim_request& q) {
            q.video = SYNA_TRIM_VIDEO_ENCODE;
            q.video_quality = quality;
            q.video_bitrate_kbps = kbps;
            q.video_max_bitrate_kbps = max_kbps;
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        return probe(out).video.bit_rate;
    };
    const int64_t q18 = video_size(18, 0, 0, "q18.mp4"), q23 = video_size(23, 0, 0, "q23.mp4"), q28 = video_size(28, 0, 0, "q28.mp4");
    CHECK_MSG(q18 > q23 && q23 > q28, "bitrates {} {} {}", q18, q23, q28);
    const int64_t capped = video_size(0, 2000, 3000, "rate2m.mp4");
    CHECK_MSG(capped > 500000 && capped < 3300000, "bitrate {}", capped);
}

TEST(trim_reencode_keeps_variable_frame_timing_and_depth) {
    {
        const std::wstring out = fresh_output("enc_vfr.mp4");
        JobResult r = trim("I_h264_vfr.mp4", out, 1000000, 2000000, SYNA_TRIM_EXACT, [](syna_trim_request& q) { q.video = SYNA_TRIM_VIDEO_ENCODE; });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED, "{}", r.json);
        Decoded src = decode_all(syna_test::fixture("I_h264_vfr.mp4"), 200, false);
        Decoded dst = decode_all(out, 100000, false);
        const int64_t start = json_int(r.json, "actualStartUs");
        size_t first = 0;
        while (first < src.pts_us.size() && src.pts_us[first] < start) ++first;
        CHECK(dst.pts_us.size() > 50);
        for (size_t i = 1; i < dst.pts_us.size(); ++i)
            CHECK_MSG(std::llabs((dst.pts_us[i] - dst.pts_us[i - 1]) - (src.pts_us[first + i] - src.pts_us[first + i - 1])) <= 1, "interval at {}", i);
    }
    {
        const std::wstring out = fresh_output("enc_hevc10.mp4");
        JobResult r = trim("J_hevc_main10.mp4", out, 1000000, 2500000, SYNA_TRIM_QUICK, [](syna_trim_request& q) { q.video = SYNA_TRIM_VIDEO_ENCODE; });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{}", r.json);
        const MediaInfo m = probe(out);
        CHECK(m.video.codec == "hevc" && m.video.bit_depth == 10 && m.video.color_transfer == "bt709");
    }
}

TEST(trim_requested_codec_is_used_or_the_trim_fails) {
    {
        const std::wstring out = fresh_output("enc_av1.mp4");
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2500000, 3500000, SYNA_TRIM_EXACT, [](syna_trim_request& q) {
            q.video = SYNA_TRIM_VIDEO_ENCODE;
            q.video_codec = SYNA_VIDEO_CODEC_AV1;
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        CHECK(probe(out).video.codec == "av1");
    }
    {  // WebM cannot carry H.264: refused, not silently re-encoded as something else.
        const std::wstring out = fresh_output("enc_h264.webm");
        JobResult r = trim("N_vp9_opus.webm", out, 1000000, 2000000, SYNA_TRIM_EXACT, [](syna_trim_request& q) {
            q.video = SYNA_TRIM_VIDEO_ENCODE;
            q.video_codec = SYNA_VIDEO_CODEC_H264;
        });
        CHECK_MSG(r.status.state == SYNA_JOB_FAILED && r.status.error == SYNA_E_UNSUPPORTED && has(r.json, "h264"), "{}", r.json);
        CHECK(!std::filesystem::exists(out));
    }
}

TEST(trim_lossless_reencode_reproduces_the_source_pixels) {
    auto wanted = [](size_t i) { return i == 151 || i == 191 || i == 239; };
    Decoded src = decode_all(syna_test::fixture("A_h264_1080p60_2trk.mp4"), 240, true, wanted);
    for (int codec : {SYNA_VIDEO_CODEC_HEVC, SYNA_VIDEO_CODEC_H264}) {
        const std::wstring out = fresh_output(codec == SYNA_VIDEO_CODEC_HEVC ? "lossless_hevc.mp4" : "lossless_h264.mp4");
        JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2516667, 4000000, SYNA_TRIM_SMART, [codec](syna_trim_request& q) {
            q.video = SYNA_TRIM_VIDEO_LOSSLESS;
            q.video_codec = codec;
        });
        CHECK_MSG(r.status.state == SYNA_JOB_SUCCEEDED && has(r.json, "\"passed\":true"), "{} {}", r.status.error, r.json);
        CHECK(has(r.json, "\"losslessVerified\":true") && has(r.json, "\"videoHandling\":\"lossless\""));
        Decoded dst = decode_all(out, 100000, true, [](size_t i) { return i == 0 || i == 40 || i == 88; });
        CHECK(dst.corrupt == 0 && dst.pts_us.size() == 89);
        CHECK_MSG(psnr(dst.luma[0], src.luma[151]) == 999.0 && psnr(dst.luma[40], src.luma[191]) == 999.0 && psnr(dst.luma[88], src.luma[239]) == 999.0,
                  "codec {} is not lossless", codec);
    }
    // No AV1 encoder here encodes losslessly: the request fails instead of producing a lossy file.
    const std::wstring out = fresh_output("lossless_av1.mp4");
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 2516667, 3000000, SYNA_TRIM_EXACT, [](syna_trim_request& q) {
        q.video = SYNA_TRIM_VIDEO_LOSSLESS;
        q.video_codec = SYNA_VIDEO_CODEC_AV1;
    });
    CHECK_MSG(r.status.state == SYNA_JOB_FAILED && has(r.json, "lossless"), "{}", r.json);
    CHECK(!std::filesystem::exists(out));
}

TEST(trim_refuses_existing_destination) {
    const std::wstring out = fresh_output("exists.mp4");
    { std::ofstream(out) << "x"; }
    JobResult r = trim("A_h264_1080p60_2trk.mp4", out, 0, 1000000, SYNA_TRIM_QUICK);
    CHECK(r.status.state == SYNA_JOB_FAILED && r.status.error == SYNA_E_BUSY);
    CHECK(std::filesystem::file_size(out) == 1);
}

TEST(trim_cancel_leaves_no_files) {
    const std::wstring src = syna_test::fixture("H_h264_longgop_bframes.mp4");
    const std::wstring out = fresh_output("cancel.mp4");
    syna_trim_request req{};
    req.source = src.c_str();
    req.destination = out.c_str();
    req.in_us = 0;
    req.out_us = 20000000;
    req.mode = SYNA_TRIM_EXACT;
    req.allow_hw_encode = 1;
    syna_job* job = nullptr;
    CHECK(syna_trim_start(&req, &job) == SYNA_OK);
    std::this_thread::sleep_for(300ms);
    syna_job_cancel(job);
    JobResult r = wait_job(job);
    CHECK(r.status.state == SYNA_JOB_CANCELLED);
    CHECK(!std::filesystem::exists(out));
    CHECK(!std::filesystem::exists(out + L".partial"));
}

TEST(waveform_envelopes_match_track_content) {
    const std::wstring out = fresh_output("A.synw");
    syna_job* job = nullptr;
    CHECK(syna_waveform_start(syna_test::fixture("A_h264_1080p60_2trk.mp4").c_str(), out.c_str(), 100, &job) == SYNA_OK);
    JobResult r = wait_job(job);
    CHECK(r.status.state == SYNA_JOB_SUCCEEDED);
    std::ifstream f(out, std::ios::binary);
    char magic[4];
    uint32_t version, tracks, bps;
    int64_t duration;
    f.read(magic, 4).read(reinterpret_cast<char*>(&version), 4).read(reinterpret_cast<char*>(&tracks), 4).read(reinterpret_cast<char*>(&bps), 4);
    f.read(reinterpret_cast<char*>(&duration), 8);
    CHECK(std::string(magic, 4) == "SYNW" && version == 1 && tracks == 2 && bps == 100);
    std::vector<std::vector<uint16_t>> data(tracks);
    for (uint32_t t = 0; t < tracks; ++t) {
        uint32_t n;
        f.read(reinterpret_cast<char*>(&n), 4);
        CHECK_NEAR(n, 1001, 2);
        data[t].resize(n * 2);
        f.read(reinterpret_cast<char*>(data[t].data()), n * 4);
    }
    // Track 1: steady 440 Hz tone (~-21 dBFS). Track 2: 1 kHz beeps during the first half of each second.
    CHECK_NEAR(data[0][2 * 300] / 65535.0, 0.088, 0.01);
    CHECK(data[1][2 * 320] / 65535.0 > 0.2);
    CHECK(data[1][2 * 370] / 65535.0 < 0.01);
}

TEST(thumbnail_is_written_at_requested_width) {
    const std::wstring out = fresh_output("thumb.jpg");
    CHECK(syna_make_thumbnail(syna_test::fixture("A_h264_1080p60_2trk.mp4").c_str(), out.c_str(), 320, -1) == SYNA_OK);
    CHECK(std::filesystem::file_size(out) > 2000);
    const std::wstring hdr = fresh_output("thumb10.jpg");
    CHECK(syna_make_thumbnail(syna_test::fixture("J_hevc_main10.mp4").c_str(), hdr.c_str(), 320, 2000000) == SYNA_OK);
}

TEST(system_info_lists_hardware) {
    char* json = nullptr;
    CHECK(syna_query_system_json(&json) == SYNA_OK);
    std::string s(json);
    syna_free_string(json);
    CHECK(has(s, "\"hardwareDecoders\":["));
    CHECK(has(s, "LGPL"));
    // Every usable video encoder, with whether it can encode losslessly. NVENC's HEVC encoder can;
    // machines without NVENC list only other encoders.
    CHECK(has(s, "\"videoEncoders\":[{"));
    if (has(s, "\"name\":\"hevc_nvenc\""))
        CHECK_MSG(has(s, "\"name\":\"hevc_nvenc\",\"label\":\"NVIDIA NVENC (HEVC)\",\"hardware\":true,\"lossless\":true"), "{}", s);
}
