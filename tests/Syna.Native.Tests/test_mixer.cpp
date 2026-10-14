#include "test.h"

#include "audio/mixer.h"

#include <algorithm>
#include <memory>

using namespace syna;

namespace {

constexpr int kRate = 48000;
constexpr int kCh = 2;

struct MixRig {
    AudioOutput output;  // not started: only used for underrun accounting
    MixSettings settings;
    MixMeters meters;
    std::vector<std::unique_ptr<AudioTrack>> tracks;
    std::unique_ptr<Mixer> mixer;

    explicit MixRig(std::vector<float> levels, int seconds = 2) {
        std::vector<AudioTrack*> raw;
        for (size_t i = 0; i < levels.size(); ++i) {
            auto t = std::make_unique<AudioTrack>(nullptr, nullptr);
            t->ordinal = static_cast<int>(i);
            t->fifo.configure(kCh, 96);
            std::vector<float> data(static_cast<size_t>(kRate) * seconds * kCh, levels[i]);
            t->fifo.write(data.data(), kRate * seconds, 0);
            t->eos.store(true);
            raw.push_back(t.get());
            tracks.push_back(std::move(t));
        }
        mixer = std::make_unique<Mixer>(raw, settings, meters, kCh, kRate, nullptr, output);
        mixer->reset(0);
    }

    std::vector<float> render(int frames) {
        std::vector<float> out(static_cast<size_t>(frames) * kCh);
        AudioChunkInfo info;
        mixer->render(out.data(), frames, info);
        return out;
    }
};

float last(const std::vector<float>& v) { return v.back(); }
float peak(const std::vector<float>& v) {
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

}  // namespace

TEST(mixer_sums_tracks_with_independent_gain) {
    MixRig rig({0.25f, 0.1f});
    rig.settings.tracks[1].gain = 0.5f;
    rig.render(480);  // gain ramp settles within one buffer
    auto out = rig.render(480);
    CHECK_NEAR(out[0], 0.25 + 0.05, 1e-5);
    CHECK_NEAR(last(out), 0.30, 1e-5);
}

TEST(mixer_mute_and_solo) {
    MixRig rig({0.2f, 0.1f, 0.05f});
    rig.settings.tracks[0].mute = true;
    rig.render(480);
    CHECK_NEAR(last(rig.render(480)), 0.15, 1e-5);
    rig.settings.tracks[0].mute = false;
    rig.settings.tracks[2].solo = true;
    rig.render(480);
    CHECK_NEAR(last(rig.render(480)), 0.05, 1e-5);
    rig.settings.tracks[1].solo = true;  // two soloed tracks
    rig.render(480);
    CHECK_NEAR(last(rig.render(480)), 0.15, 1e-5);
    rig.settings.master_mute = true;
    rig.render(480);
    CHECK_NEAR(peak(rig.render(480)), 0.0, 1e-7);
}

TEST(mixer_gain_changes_are_ramped) {
    MixRig rig({0.5f});
    rig.render(480);
    rig.settings.tracks[0].gain = 0.0f;
    auto out = rig.render(480);
    // First sample barely moved, last sample reached the target: no step discontinuity.
    CHECK(out[0] > 0.49f);
    CHECK_NEAR(last(out), 0.0, 1e-6);
}

TEST(mixer_limiter_prevents_clipping_when_tracks_overlap) {
    MixRig rig({0.8f, 0.8f});
    auto out = rig.render(4800);
    CHECK(peak(out) <= 0.9661f);
    CHECK(last(out) > 0.95f);
    CHECK(rig.meters.gain_reduction_db.load() > 4.0f);
}

TEST(mixer_without_limiter_hard_clips_to_full_scale) {
    MixRig rig({0.8f, 0.8f});
    rig.settings.limiter = false;
    auto out = rig.render(4800);
    CHECK_NEAR(peak(out), 1.0, 1e-6);
}

TEST(mixer_track_offset_delays_track) {
    MixRig rig({0.0f, 0.3f});
    rig.settings.tracks[1].offset_us = 10000;  // 10 ms = 480 samples
    auto out = rig.render(960);
    CHECK_NEAR(out[(479) * kCh], 0.0, 1e-7);
    CHECK_NEAR(out[(481) * kCh], 0.3, 1e-5);
}

TEST(mixer_meters_report_post_gain_peaks) {
    MixRig rig({0.5f, 0.25f});
    rig.settings.tracks[0].gain = 0.5f;
    rig.render(480);
    rig.meters.track_peak[0] = 0;
    rig.meters.track_peak[1] = 0;
    rig.render(480);
    CHECK_NEAR(rig.meters.track_peak[0].load(), 0.25, 1e-5);
    CHECK_NEAR(rig.meters.track_peak[1].load(), 0.25, 1e-5);
}

TEST(sample_fifo_absorbs_jitter_and_fills_gaps) {
    SampleFifo f;
    f.configure(1, 4);
    std::vector<float> a(100, 1.0f), b(100, 2.0f);
    f.write(a.data(), 100, 0);
    f.write(b.data(), 100, 102);   // 2-sample jitter: appended contiguously
    CHECK(f.end_pos() == 200);
    f.write(a.data(), 100, 300);   // real gap: 100 samples of silence inserted
    CHECK(f.end_pos() == 400);
    std::vector<float> out(400);
    CHECK(f.read(out.data(), 400, 0) == 400);
    CHECK(out[99] == 1.0f && out[100] == 2.0f && out[250] == 0.0f && out[350] == 1.0f);
}
