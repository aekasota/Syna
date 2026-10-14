#include "test.h"

#include "common/ffmpeg.h"
#include "syna_api.h"
#include "media/media_info.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

// Opt-in checks against a real recording: set SYNA_REAL_CLIP to a ShadowPlay file. The source is only
// read; outputs go to the temp folder.
namespace {

std::string run_trim(const std::wstring& src, const char* name, int64_t in_us, int64_t out_us, int mode, syna_job_status& st,
                     const std::function<void(syna_trim_request&)>& set = {}) {
    std::wstring out = syna_test::temp_path(name);
    std::filesystem::remove(out);
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
    for (;;) {
        syna_job_status_get(job, &st);
        if (st.state != SYNA_JOB_RUNNING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    char* json = nullptr;
    syna_job_result_json(job, &json);
    std::string s = json ? json : "";
    syna_free_string(json);
    syna_job_destroy(job);
    return s;
}

// RMS over every channel and sample of one audio stream (by order among the audio streams).
double audio_rms(const std::wstring& path, int ordinal) {
    std::string err;
    syna::FormatPtr fmt = syna::open_input(path, true, &err);
    CHECK_MSG(fmt != nullptr, "open {}", err);
    int si = -1;
    for (unsigned i = 0, seen = 0; i < fmt->nb_streams && si < 0; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && static_cast<int>(seen++) == ordinal) si = static_cast<int>(i);
    CHECK(si >= 0);
    AVStream* st = fmt->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    syna::CodecPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    CHECK(avcodec_open2(ctx.get(), codec, nullptr) >= 0);
    syna::PacketPtr pkt = syna::make_packet();
    syna::FramePtr f = syna::make_frame();
    double sum = 0;
    int64_t n = 0;
    auto take = [&] {
        while (avcodec_receive_frame(ctx.get(), f.get()) >= 0) {
            CHECK(f->format == AV_SAMPLE_FMT_FLTP);  // AAC
            for (int c = 0; c < f->ch_layout.nb_channels; ++c)
                for (int i = 0; i < f->nb_samples; ++i) {
                    const double v = reinterpret_cast<const float*>(f->extended_data[c])[i];
                    sum += v * v;
                    ++n;
                }
            av_frame_unref(f.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == si) avcodec_send_packet(ctx.get(), pkt.get());
        av_packet_unref(pkt.get());
        take();
    }
    avcodec_send_packet(ctx.get(), nullptr);
    take();
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0;
}

}  // namespace

TEST(real_clip_trims_in_every_mode) {
    const std::wstring src = syna_test::real_clip();
    const auto before = std::filesystem::last_write_time(src);
    syna::MediaInfo info;
    CHECK(syna::probe_media(src, true, info) == SYNA_OK);
    const int64_t in = info.duration_us / 3 + 8333;  // deliberately between keyframes
    const int64_t out = in + 4000000;
    for (int mode : {SYNA_TRIM_QUICK, SYNA_TRIM_SMART, SYNA_TRIM_EXACT}) {
        syna_job_status st{};
        const char* names[] = {"real_quick.mp4", "real_smart.mp4", "real_exact.mp4"};
        const auto t0 = std::chrono::steady_clock::now();
        std::string json = run_trim(src, names[mode], in, out, mode, st);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("      mode %d: %.2f s  %s\n", mode, s, json.c_str());
        CHECK_MSG(st.state == SYNA_JOB_SUCCEEDED, "mode {} failed: {}", mode, json);
        CHECK(json.find("\"passed\":true") != std::string::npos);
        // Several source tracks arrive as one mixed track; a single track is copied.
        syna::MediaInfo o;
        CHECK(syna::probe_media(syna_test::temp_path(names[mode]), true, o) == SYNA_OK);
        CHECK(o.audio.size() == std::min<size_t>(info.audio.size(), 1));
        if (info.audio.size() >= 2) CHECK(json.find("\"audioMethod\":\"mix\"") != std::string::npos);
    }
    CHECK(std::filesystem::last_write_time(src) == before);
}

TEST(real_clip_keeps_tracks_separate_and_applies_output_settings) {
    const std::wstring src = syna_test::real_clip();
    const auto before = std::filesystem::last_write_time(src);
    syna::MediaInfo info;
    CHECK(syna::probe_media(src, true, info) == SYNA_OK);
    const int64_t in = info.duration_us / 2 + 8333, out = in + 3000000;
    auto check = [&](const char* name, int mode, const std::function<void(syna_trim_request&)>& set) {
        syna_job_status st{};
        const auto t0 = std::chrono::steady_clock::now();
        std::string json = run_trim(src, name, in, out, mode, st, set);
        std::printf("      %s: %.2f s  %s\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), json.c_str());
        CHECK_MSG(st.state == SYNA_JOB_SUCCEEDED && json.find("\"passed\":true") != std::string::npos, "{} failed: {}", name, json);
        syna::MediaInfo o;
        CHECK(syna::probe_media(syna_test::temp_path(name), true, o) == SYNA_OK);
        return std::make_pair(o, json);
    };

    // Separate tracks in every mode: the source's tracks, each copied in its own format.
    const char* names[] = {"real_sep_quick.mp4", "real_sep_smart.mp4", "real_sep_exact.mp4"};
    for (int mode : {SYNA_TRIM_QUICK, SYNA_TRIM_SMART, SYNA_TRIM_EXACT}) {
        auto [o, json] = check(names[mode], mode, [](syna_trim_request& r) { r.audio = SYNA_TRIM_AUDIO_SEPARATE; });
        CHECK_MSG(o.audio.size() == info.audio.size(), "{} audio tracks, source {}", o.audio.size(), info.audio.size());
        for (size_t i = 0; i < o.audio.size(); ++i)
            CHECK(o.audio[i].codec == info.audio[i].codec && o.audio[i].sample_rate == info.audio[i].sample_rate && o.audio[i].channels == info.audio[i].channels);
        CHECK(json.find("\"audioMethod\":\"copy\"") != std::string::npos);
    }

    // Balanced in Quick Cut: the whole selection re-encoded at the source's size and frame rate, smaller.
    {
        auto [o, json] = check("real_balanced.mp4", SYNA_TRIM_QUICK, [](syna_trim_request& r) {
            r.video = SYNA_TRIM_VIDEO_ENCODE;
            r.video_quality = 23;
            r.audio_codec = SYNA_AUDIO_CODEC_AAC;
            r.audio_bitrate_kbps = 192;
        });
        CHECK(o.video.codec == info.video.codec && o.video.width == info.video.width && o.video.height == info.video.height);
        CHECK_MSG(std::fabs(o.video.fps_avg - info.video.fps_avg) < 0.5, "fps {} vs source {}", o.video.fps_avg, info.video.fps_avg);
        CHECK_MSG(o.video.bit_rate < info.video.bit_rate, "bitrate {} vs source {}", o.video.bit_rate, info.video.bit_rate);
        CHECK(json.find("\"modeUsed\":\"exact\"") != std::string::npos && o.audio.size() == 1);
    }

    // Custom: HEVC, 720p, 30 fps.
    {
        auto [o, json] = check("real_720p30_hevc.mp4", SYNA_TRIM_SMART, [](syna_trim_request& r) {
            r.video = SYNA_TRIM_VIDEO_ENCODE;
            r.video_codec = SYNA_VIDEO_CODEC_HEVC;
            r.video_height = 720;
            r.video_fps_num = 30;
            r.video_fps_den = 1;
        });
        CHECK(o.video.codec == "hevc" && o.video.height == 720 && std::fabs(o.video.fps_avg - 30.0) < 0.05);
    }

    // Lossless HEVC: verified against the source's pixels by the engine itself.
    {
        auto [o, json] = check("real_lossless.mp4", SYNA_TRIM_EXACT, [](syna_trim_request& r) {
            r.video = SYNA_TRIM_VIDEO_LOSSLESS;
            r.video_codec = SYNA_VIDEO_CODEC_HEVC;
        });
        CHECK(o.video.codec == "hevc" && json.find("\"losslessVerified\":true") != std::string::npos);
    }
    CHECK(std::filesystem::last_write_time(src) == before);
}

TEST(real_clip_exports_the_mixer) {
    // As the app sends a mixer with the microphone (track 2) muted: kept separate, the desktop track
    // is copied and the microphone stays a silent track of its own, in every mode; mixed with the
    // desktop at half level, the one track is the desktop alone, 6 dB down.
    const std::wstring src = syna_test::real_clip();
    const auto before = std::filesystem::last_write_time(src);
    syna::MediaInfo info;
    CHECK(syna::probe_media(src, true, info) == SYNA_OK);
    if (info.audio.size() < 2) throw syna_test::Skip("needs a recording with two audio tracks");
    const int64_t in = info.duration_us / 3 + 8333, out = in + 4000000;
    const std::vector<syna_trim_track> no_mic = {{info.audio[1].stream_index, 0.0f, 0}};
    const std::vector<syna_trim_track> half_desktop_no_mic = {{info.audio[0].stream_index, 0.5f, 0}, {info.audio[1].stream_index, 0.0f, 0}};
    auto run = [&](const char* name, int mode, int audio, const std::vector<syna_trim_track>& tracks) {
        syna_job_status st{};
        const auto t0 = std::chrono::steady_clock::now();
        std::string json = run_trim(src, name, in, out, mode, st, [&](syna_trim_request& r) {
            r.audio = audio;
            r.tracks = tracks.data();
            r.track_count = static_cast<int32_t>(tracks.size());
        });
        std::printf("      %s: %.2f s  %s\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), json.c_str());
        CHECK_MSG(st.state == SYNA_JOB_SUCCEEDED && json.find("\"passed\":true") != std::string::npos, "{} failed: {}", name, json);
        return json;
    };

    const char* names[] = {"real_mixer_sep_quick.mp4", "real_mixer_sep_smart.mp4", "real_mixer_sep_exact.mp4"};
    const char* video[] = {"\"videoMethod\":\"copy\"", "\"videoMethod\":\"smart\"", "\"videoMethod\":\"reencode\""};
    double desktop = 0;
    for (int mode : {SYNA_TRIM_QUICK, SYNA_TRIM_SMART, SYNA_TRIM_EXACT}) {
        const std::string json = run(names[mode], mode, SYNA_TRIM_AUDIO_SEPARATE, no_mic);
        CHECK_MSG(json.find("\"audioMethod\":\"copy+encode\"") != std::string::npos && json.find(video[mode]) != std::string::npos, "{}", json);
        syna::MediaInfo o;
        CHECK(syna::probe_media(syna_test::temp_path(names[mode]), true, o) == SYNA_OK);
        CHECK(o.audio.size() == 2);
        desktop = audio_rms(syna_test::temp_path(names[mode]), 0);
        const double mic = audio_rms(syna_test::temp_path(names[mode]), 1);
        std::printf("      mode %d: desktop RMS %.5f, muted microphone RMS %.7f\n", mode, desktop, mic);
        CHECK_MSG(desktop > 0.001 && mic < 0.0001, "desktop {} microphone {}", desktop, mic);
    }
    const std::string json = run("real_mixer_mix.mp4", SYNA_TRIM_EXACT, SYNA_TRIM_AUDIO_MIX, half_desktop_no_mic);
    CHECK(json.find("\"audioMethod\":\"mix\"") != std::string::npos && json.find("\"adjusted\":true") != std::string::npos);
    const double mix = audio_rms(syna_test::temp_path("real_mixer_mix.mp4"), 0);
    const double db = 20 * std::log10(mix / desktop);  // against the copied desktop track of the same Exact cut
    std::printf("      mix RMS %.5f = desktop %+.2f dB\n", mix, db);
    CHECK_MSG(std::fabs(db + 6.02) < 0.5, "mix {} vs desktop {}: {:.2f} dB", mix, desktop, db);
    CHECK(std::filesystem::last_write_time(src) == before);
}
