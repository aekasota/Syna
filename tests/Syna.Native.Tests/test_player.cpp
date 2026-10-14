#include "test.h"

#include "syna_api.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Drives the real C ABI the way the UI does: commands in, polled state and events out.
struct PlayerRig {
    syna_player* p = nullptr;
    std::vector<syna_event> seen;

    PlayerRig() {
        syna_result r = syna_player_create(&p);
        CHECK_MSG(r == SYNA_OK, "create {} {}", r, syna_last_error());
        syna_player_resize(p, 1280, 720, 1.0f, 1.0f);
        syna_player_set_master_mute(p, 1);  // tests run silently; meters are pre-master
    }
    ~PlayerRig() { syna_player_destroy(p); }

    void pump() {
        syna_event ev[32];
        int n = syna_player_poll_events(p, ev, 32);
        seen.insert(seen.end(), ev, ev + n);
    }

    syna_event wait_event(int type, std::chrono::milliseconds timeout = 5000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            pump();
            for (size_t i = 0; i < seen.size(); ++i) {
                if (seen[i].type == type) {
                    syna_event e = seen[i];
                    seen.erase(seen.begin(), seen.begin() + static_cast<ptrdiff_t>(i) + 1);
                    return e;
                }
                if (seen[i].type == SYNA_EVENT_OPEN_FAILED && type != SYNA_EVENT_OPEN_FAILED)
                    throw syna_test::Failure(std::format("open failed: {}", seen[i].code));
            }
            if (std::chrono::steady_clock::now() > deadline) throw syna_test::Failure(std::format("timeout waiting for event {}", type));
            std::this_thread::sleep_for(2ms);
        }
    }

    syna_player_state state() {
        syna_player_state s;
        syna_player_get_state(p, &s);
        return s;
    }

    void open(const char* fixture, int64_t start_us = 0) {
        syna_open_options o{};
        o.start_us = start_us;
        o.hw_decode = 1;
        syna_player_open(p, syna_test::fixture(fixture).c_str(), &o);
        wait_event(SYNA_EVENT_OPENED);
        wait_event(SYNA_EVENT_SEEK_DONE);
    }

    int64_t seek(int64_t us, int mode = SYNA_SEEK_EXACT) {
        seen.clear();
        syna_player_seek(p, us, mode);
        return wait_event(SYNA_EVENT_SEEK_DONE).value;
    }

    int64_t step(int n) {
        seen.clear();
        syna_player_step(p, n);
        return wait_event(SYNA_EVENT_SEEK_DONE).value;
    }
};

constexpr int64_t kFrame60 = 16667;

}  // namespace

TEST(player_presents_first_frame_with_hardware_decoder) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4");
    syna_player_state s = rig.state();
    CHECK(s.state == SYNA_STATE_PAUSED);
    CHECK(s.frame_pts_us == 0);
    CHECK(s.audio_tracks == 2);
    CHECK_NEAR(s.duration_us / 1e6, 10.0, 0.05);
    CHECK_MSG(s.decoder == SYNA_DECODER_D3D11VA, "decoder {}", s.decoder);
    char* json = nullptr;
    CHECK(syna_player_media_json(rig.p, &json) == SYNA_OK);
    CHECK(std::string(json).find("\"keyframesUs\":[0,") != std::string::npos);
    syna_free_string(json);
}

TEST(player_exact_seek_lands_on_frame_containing_target) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4");
    // 2.51 s falls inside frame 150 (2.500000 .. 2.516667).
    CHECK_NEAR(rig.seek(2510000), 2500000, 1);
    CHECK_NEAR(rig.seek(2516700), 2516667, 1);
    // Target before a keyframe boundary: frame 149 lives in the previous GOP.
    CHECK_NEAR(rig.seek(2499000), 2483333, 1);
    // Fast seek presents the keyframe at or before the target.
    CHECK_NEAR(rig.seek(2990000, SYNA_SEEK_FAST), 2500000, 1);
}

TEST(player_frame_step_forward_and_backward) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4");
    CHECK_NEAR(rig.seek(2500000), 2500000, 1);
    CHECK_NEAR(rig.step(1), 2500000 + kFrame60, 1);
    CHECK_NEAR(rig.step(1), 2500000 + 2 * kFrame60, 1);
    CHECK_NEAR(rig.step(-1), 2500000 + kFrame60, 1);
    CHECK_NEAR(rig.step(-1), 2500000, 1);
    // Crosses into the previous GOP (keyframe at 2.0 s): requires decoding forward from it.
    CHECK_NEAR(rig.step(-1), 2500000 - kFrame60, 1);
}

TEST(player_steps_follow_vfr_timestamps) {
    PlayerRig rig;
    rig.open("I_h264_vfr.mp4");
    const int64_t a = rig.seek(1000000);
    const int64_t b = rig.step(1);
    const int64_t c = rig.step(1);
    CHECK(b > a && c > b);
    // Jittered cadence: consecutive intervals differ, i.e. timestamps are not re-timed to CFR.
    CHECK((b - a) != (c - b));
    CHECK_NEAR(rig.step(-1), b, 0);
}

TEST(player_long_gop_with_bframes_is_frame_accurate) {
    PlayerRig rig;
    rig.open("H_h264_longgop_bframes.mp4");
    const int64_t t = rig.seek(7300000);
    CHECK(t <= 7300000 && t > 7300000 - kFrame60);
    const int64_t prev = rig.step(-1);
    CHECK_NEAR(t - prev, kFrame60, 2);
    const int64_t back_across_keyframe = rig.seek(5000000 - 1);
    CHECK_NEAR(back_across_keyframe, 5000000 - kFrame60, 2);
}

TEST(player_plays_in_real_time_with_stable_sync) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4");
    syna_player_play(rig.p);
    std::this_thread::sleep_for(300ms);
    const syna_player_state a = rig.state();
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(1500ms);
    const syna_player_state b = rig.state();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(b.state == SYNA_STATE_PLAYING);
    CHECK_NEAR((b.position_us - a.position_us) / 1e6, wall, 0.06);
    CHECK_MSG(std::fabs(b.av_drift_ms) < 20.0, "drift {} ms", b.av_drift_ms);
    CHECK_MSG(b.frames_dropped <= 2, "dropped {}", b.frames_dropped);
    CHECK(b.frames_presented - a.frames_presented >= 80);
    CHECK_MSG(b.track_peak[0] > 0.05f, "track peaks {} {}", b.track_peak[0], b.track_peak[1]);  // 440 Hz tone, -21 dBFS
}

TEST(player_playback_rate_changes_clock_speed) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4");
    syna_player_set_rate(rig.p, 2.0);
    syna_player_play(rig.p);
    std::this_thread::sleep_for(400ms);
    const syna_player_state a = rig.state();
    std::this_thread::sleep_for(1000ms);
    const syna_player_state b = rig.state();
    CHECK_NEAR((b.position_us - a.position_us) / 1e6, 2.0, 0.15);
    syna_player_set_rate(rig.p, 0.5);
    std::this_thread::sleep_for(400ms);
    const syna_player_state c = rig.state();
    std::this_thread::sleep_for(1000ms);
    const syna_player_state d = rig.state();
    CHECK_NEAR((d.position_us - c.position_us) / 1e6, 0.5, 0.08);
}

TEST(player_mute_and_solo_affect_meters) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4");
    syna_player_set_track_mute(rig.p, 0, 1);
    syna_player_play(rig.p);
    std::this_thread::sleep_for(700ms);
    rig.state();  // reset peak window
    std::this_thread::sleep_for(1100ms);
    syna_player_state s = rig.state();
    CHECK(s.track_peak[0] < 1e-6f);
    CHECK(s.track_peak[1] > 0.1f);  // mic beeps every second
    syna_player_set_track_mute(rig.p, 0, 0);
    syna_player_set_track_solo(rig.p, 0, 1);
    std::this_thread::sleep_for(300ms);
    rig.state();
    std::this_thread::sleep_for(1100ms);
    s = rig.state();
    CHECK(s.track_peak[0] > 0.05f);
    CHECK(s.track_peak[1] < 1e-6f);
}

TEST(player_plays_to_end_and_reports_ended) {
    PlayerRig rig;
    rig.open("G_h264_allintra_short.mp4", 1500000);
    syna_player_play(rig.p);
    rig.wait_event(SYNA_EVENT_ENDED, 3000ms);
    CHECK(rig.state().state == SYNA_STATE_ENDED);
}

TEST(player_ab_loop_wraps_around) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4", 1000000);
    syna_player_set_loop(rig.p, 1, 1000000, 1400000);
    syna_player_play(rig.p);
    rig.wait_event(SYNA_EVENT_LOOPED, 2000ms);
    std::this_thread::sleep_for(150ms);
    const syna_player_state s = rig.state();
    CHECK(s.position_us >= 1000000 && s.position_us < 1400000 + 50000);
}

TEST(player_decodes_other_codecs_and_containers) {
    PlayerRig rig;
    const char* files[] = {"B_hevc_1080p60_2trk.mp4", "J_hevc_main10.mp4", "L_av1_1080p60_2trk.mp4", "M_h264_2trk.mkv",
                           "N_vp9_opus.webm", "E_h264_mixed_rates.mp4", "D_h264_4trk.mp4"};
    for (const char* f : files) {
        rig.seen.clear();
        rig.open(f);
        const syna_player_state s = rig.state();
        CHECK_MSG(s.frame_pts_us >= 0, "{} no frame", f);
        CHECK_MSG(s.decoder != SYNA_DECODER_NONE, "{} no decoder", f);
        CHECK_NEAR(rig.seek(1000000), 1000000, 34000);
    }
}

TEST(player_reports_open_failures) {
    PlayerRig rig;
    syna_open_options o{};
    o.hw_decode = 1;
    syna_player_open(rig.p, syna_test::fixture("K1_truncated.mp4").c_str(), &o);
    syna_event e = rig.wait_event(SYNA_EVENT_OPEN_FAILED);
    CHECK(e.code != SYNA_OK);
    syna_player_open(rig.p, L"C:\\definitely\\missing\\clip.mp4", &o);
    e = rig.wait_event(SYNA_EVENT_OPEN_FAILED);
    CHECK(e.code == SYNA_E_NOT_FOUND);
    // Still usable afterwards.
    rig.open("C_h264_720p30_1trk.mp4");
    CHECK(rig.state().frame_pts_us == 0);
}

TEST(player_survives_corrupt_data) {
    PlayerRig rig;
    rig.open("K2_corrupt_middle.mp4", 3500000);
    syna_player_set_rate(rig.p, 2.0);
    syna_player_play(rig.p);
    std::this_thread::sleep_for(1500ms);
    const syna_player_state s = rig.state();
    CHECK(s.state == SYNA_STATE_PLAYING || s.state == SYNA_STATE_ENDED);
    CHECK(s.position_us > 4500000);
}

TEST(player_software_decode_fallback_works) {
    PlayerRig rig;
    syna_open_options o{};
    o.hw_decode = 0;
    syna_player_open(rig.p, syna_test::fixture("A_h264_1080p60_2trk.mp4").c_str(), &o);
    rig.wait_event(SYNA_EVENT_OPENED);
    rig.wait_event(SYNA_EVENT_SEEK_DONE);
    CHECK(rig.state().decoder == SYNA_DECODER_SOFTWARE);
    CHECK_NEAR(rig.seek(2510000), 2500000, 1);
    CHECK_NEAR(rig.step(-1), 2500000 - kFrame60, 1);
}

TEST(player_saves_current_frame) {
    PlayerRig rig;
    rig.open("A_h264_1080p60_2trk.mp4", 2000000);
    const std::wstring png = syna_test::temp_path("frame.png");
    const std::wstring jpg = syna_test::temp_path("frame.jpg");
    std::filesystem::remove(png);
    syna_result r = syna_player_save_frame(rig.p, png.c_str(), 0);
    CHECK_MSG(r == SYNA_OK, "png {} {}", r, syna_last_error());
    r = syna_player_save_frame(rig.p, jpg.c_str(), 1);
    CHECK_MSG(r == SYNA_OK, "jpg {} {}", r, syna_last_error());
    CHECK(std::filesystem::file_size(png) > 100000);
    CHECK(std::filesystem::file_size(jpg) > 20000);
}
