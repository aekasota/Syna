#include "test.h"

#include "syna_api.h"
#include "media/media_info.h"

using namespace syna;

static MediaInfo probe(const char* name, bool full = true) {
    MediaInfo info;
    syna_result r = probe_media(syna_test::fixture(name), full, info);
    CHECK_MSG(r == SYNA_OK, "probe {} -> {} ({})", name, r, syna_last_error());
    return info;
}

TEST(probe_shadowplay_like_layout) {
    MediaInfo m = probe("A_h264_1080p60_2trk.mp4");
    CHECK(m.container == "MP4");
    CHECK(m.has_video);
    CHECK(m.video.codec == "h264");
    CHECK(m.video.width == 1920 && m.video.height == 1080);
    CHECK_NEAR(m.video.fps_avg, 60.0, 0.01);
    CHECK(m.video.pixel_format == "yuv420p");
    CHECK(m.video.bit_depth == 8);
    CHECK(m.video.color_space == "bt709");
    CHECK_NEAR(m.video.keyframe_interval_s, 0.5, 0.01);
    CHECK(m.video.keyframes_us.size() == 20);
    CHECK(m.video.keyframes_us.front() == 0);
    CHECK_NEAR(m.duration_us / 1e6, 10.0, 0.05);
    CHECK(m.audio.size() == 2);
    CHECK(m.audio[0].label == "System sounds" && !m.audio[0].label_is_suggestion);
    CHECK(m.audio[1].label == "Microphone" && !m.audio[1].label_is_suggestion);
    CHECK(m.audio[0].sample_rate == 48000 && m.audio[0].channels == 2);
}

TEST(probe_fast_mode_skips_decoding_but_has_geometry) {
    MediaInfo m = probe("A_h264_1080p60_2trk.mp4", false);
    CHECK(m.video.width == 1920);
    CHECK_NEAR(m.video.fps_avg, 60.0, 0.01);
    CHECK(m.audio.size() == 2);
    CHECK(m.video.keyframes_us.empty());
}

TEST(probe_suggests_labels_for_unnamed_tracks) {
    MediaInfo m = probe("F_h264_offset_track.mp4");
    CHECK(m.audio.size() == 2);
    CHECK(m.audio[0].label == "Desktop" && m.audio[0].label_is_suggestion);
    CHECK(m.audio[1].label == "Microphone" && m.audio[1].label_is_suggestion);
    MediaInfo d = probe("D_h264_4trk.mp4");
    CHECK(d.audio.size() == 4);
    CHECK(d.audio[3].label == "Track 4");
}

TEST(probe_detects_vfr_timing) {
    MediaInfo vfr = probe("I_h264_vfr.mp4");
    CHECK(vfr.video.vfr);
    CHECK(vfr.video.frame_interval_max_ms - vfr.video.frame_interval_min_ms > 4.0);
    MediaInfo cfr = probe("A_h264_1080p60_2trk.mp4");
    CHECK(!cfr.video.vfr);
}

TEST(probe_reports_10bit_and_bframes) {
    MediaInfo j = probe("J_hevc_main10.mp4");
    CHECK(j.video.codec == "hevc");
    CHECK(j.video.bit_depth == 10);
    MediaInfo h = probe("H_h264_longgop_bframes.mp4");
    CHECK(h.video.has_b_frames > 0);
    CHECK_NEAR(h.video.keyframe_interval_s, 5.0, 0.05);
}

TEST(probe_keyframes_are_display_times_with_bframes) {
    // MP4 indexes decode times; with B-frames each IDR is displayed after it is decoded. The UI and
    // the trim explanations need display times: H has an IDR every 5 s exactly.
    MediaInfo h = probe("H_h264_longgop_bframes.mp4");
    CHECK(h.video.keyframes_us.size() == 4);
    for (size_t i = 0; i < h.video.keyframes_us.size(); ++i) CHECK_NEAR(h.video.keyframes_us[i], static_cast<int64_t>(i) * 5000000, 1000);
}

TEST(probe_keyframes_follow_the_decode_delay_the_file_uses) {
    // R decodes three frames ahead but declares a reorder depth of two (NVENC with B-frames as
    // references); its IDRs are still displayed exactly every 2 s, not a frame early.
    MediaInfo r = probe("R_h264_nvenc_bref.mp4");
    CHECK(r.video.keyframes_us.size() == 4);
    for (size_t i = 0; i < r.video.keyframes_us.size(); ++i) CHECK_NEAR(r.video.keyframes_us[i], static_cast<int64_t>(i) * 2000000, 1000);
}

TEST(probe_other_containers) {
    MediaInfo mkv = probe("M_h264_2trk.mkv");
    CHECK(mkv.container == "Matroska");
    CHECK(mkv.audio.size() == 2);
    CHECK(mkv.audio[0].label == "System sounds");
    MediaInfo webm = probe("N_vp9_opus.webm");
    CHECK(webm.container == "WebM");
    CHECK(webm.video.codec == "vp9");
    CHECK(webm.audio[0].codec == "opus");
}

TEST(probe_rejects_truncated_recording) {
    MediaInfo info;
    syna_result r = probe_media(syna_test::fixture("K1_truncated.mp4"), true, info);
    CHECK(r != SYNA_OK);
    CHECK(std::string(syna_last_error()).size() > 0);
    r = probe_media(L"C:\\definitely\\missing\\clip.mp4", false, info);
    CHECK(r == SYNA_E_NOT_FOUND);
}

TEST(probe_json_is_exposed_through_abi) {
    char* json = nullptr;
    syna_result r = syna_probe_json(syna_test::fixture("A_h264_1080p60_2trk.mp4").c_str(), SYNA_PROBE_FAST, &json);
    CHECK(r == SYNA_OK && json);
    std::string s(json);
    syna_free_string(json);
    CHECK(s.front() == '{' && s.back() == '}');
    CHECK(s.find("\"label\":\"Microphone\"") != std::string::npos);
}
