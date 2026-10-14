#include "test.h"

#include "audio/mixer.h"
#include "media/pipeline.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <thread>

using namespace syna;
using namespace std::chrono_literals;

namespace {

constexpr int kRate = 48000;

// Drives the real decode -> per-track FIFO -> mixer path, pulling the mix faster than real time.
// Each block waits until every track has decoded audio buffered, so the test never measures its
// own starvation.
struct AudioRig {
    AudioOutput output;  // not started: only supplies the 48 kHz stereo mix format
    MixSettings settings;
    MixMeters meters;
    UniqueHandle frame_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::unique_ptr<Pipeline> pipeline;
    std::atomic<bool> stop{false};
    std::thread video_sink;  // discards decoded video so demuxing never waits on a full frame queue

    ~AudioRig() {
        stop.store(true);
        if (video_sink.joinable()) video_sink.join();
        pipeline.reset();
    }

    AudioRig(const std::wstring& path, int solo_track, int64_t start_us = 0) {
        if (solo_track >= 0) settings.tracks[solo_track].solo = true;
        Pipeline::Deps deps;
        deps.audio = &output;
        deps.settings = &settings;
        deps.meters = &meters;
        deps.frame_event = frame_event.get();
        pipeline = std::make_unique<Pipeline>(deps);
        std::string err;
        CHECK_MSG(pipeline->open(path, false, &err) == SYNA_OK, "open {}", err);
        pipeline->start(start_us, true);
        video_sink = std::thread([this] {
            while (!stop.load()) {
                VideoFrame f;
                if (!pipeline->frames().pop(f)) std::this_thread::sleep_for(1ms);
            }
        });
    }

    // Interleaved stereo.
    std::vector<float> render(double seconds) {
        const int block = 480;
        const int blocks = static_cast<int>(seconds * kRate / block);
        std::vector<float> out(static_cast<size_t>(blocks) * block * 2);
        AudioChunkInfo info;
        for (int b = 0; b < blocks; ++b) {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!pipeline->audio_ready()) {
                if (std::chrono::steady_clock::now() > deadline) throw syna_test::Failure("decoded audio did not arrive");
                std::this_thread::sleep_for(1ms);
            }
            pipeline->mixer()->render(out.data() + static_cast<size_t>(b) * block * 2, block, info);
        }
        return out;
    }
};

// A pure tone satisfies x[n+1] + x[n-1] = 2cos(w) x[n]. Inserted silence or cut samples break the
// recurrence with a residual near the tone's amplitude; codec noise stays orders of magnitude below.
// Returns the number of separate discontinuity events in the left channel after `skip_s`.
int discontinuities(const std::vector<float>& stereo, double freq, double skip_s) {
    const size_t frames = stereo.size() / 2;
    const size_t skip = static_cast<size_t>(skip_s * kRate);
    auto x = [&](size_t n) { return static_cast<double>(stereo[n * 2]); };
    double amp = 0;
    for (size_t n = skip; n < frames; ++n) amp = std::max(amp, std::fabs(x(n)));
    CHECK_MSG(amp > 0.01, "tone missing (peak {})", amp);
    const double k = 2 * std::cos(2 * std::numbers::pi * freq / kRate);
    int events = 0;
    bool inside = false;
    for (size_t n = skip + 1; n + 1 < frames; ++n) {
        const bool bad = std::fabs(x(n + 1) + x(n - 1) - k * x(n)) > 0.02 * amp;
        if (bad && !inside) ++events;
        inside = bad;
    }
    return events;
}

}  // namespace

TEST(audio_jittered_timestamps_play_without_gaps) {
    // Timestamps wobble +-0.6 ms per packet with 4 ms outliers (ShadowPlay-like); every decoded
    // sample must still reach the mix exactly once, in order. Placing each packet at its timestamp
    // instead would make about six audible discontinuities per second in this fixture.
    const std::wstring o = syna_test::fixture("O_aac_2trk_jitter.mp4");
    {
        AudioRig rig(o, 0);
        const int bad = discontinuities(rig.render(15.0), 440.0, 0.25);
        CHECK_MSG(bad == 0, "track 1: {} discontinuities in 15 s", bad);
        const AudioTrackStats s = rig.pipeline->audio_stats()[0];
        CHECK(s.discontinuities == 0 && s.gap_us == 0 && s.overlap_us == 0);
        CHECK_MSG(std::fabs(s.drift_ms) < 2.0, "track 1 drift {} ms", s.drift_ms);
    }
    {
        // Track 2 also drifts 1000 ppm against its timestamps: it must be stretched back into
        // place (within a few ms) instead of being cut or padded.
        AudioRig rig(o, 1);
        const int bad = discontinuities(rig.render(15.0), 1000.0, 0.25);
        CHECK_MSG(bad == 0, "track 2: {} discontinuities in 15 s", bad);
        const AudioTrackStats s = rig.pipeline->audio_stats()[1];
        CHECK(s.discontinuities == 0 && s.gap_us == 0 && s.overlap_us == 0);
        CHECK_MSG(s.stretched_us > 0, "drift correction never engaged");
        CHECK_MSG(std::fabs(s.drift_ms) < 6.0, "track 2 drift {} ms", s.drift_ms);
    }
}

TEST(audio_stays_continuous_after_seek) {
    AudioRig rig(syna_test::fixture("O_aac_2trk_jitter.mp4"), 0, 7300000);
    const int bad = discontinuities(rig.render(6.0), 440.0, 0.25);
    CHECK_MSG(bad == 0, "{} discontinuities after seeking", bad);
    CHECK(rig.pipeline->audio_stats()[0].discontinuities == 0);
}

TEST(audio_stays_continuous_across_repeated_seeks) {
    AudioRig rig(syna_test::fixture("O_aac_2trk_jitter.mp4"), 0, 2000000);
    rig.render(1.0);
    for (int64_t target : {12400000LL, 3100000LL, 15700000LL, 6200000LL}) {
        rig.pipeline->seek(target, true);
        const int bad = discontinuities(rig.render(2.0), 440.0, 0.25);
        CHECK_MSG(bad == 0, "{} discontinuities after seeking to {} us", bad, target);
    }
    CHECK(rig.pipeline->audio_stats()[0].discontinuities == 0);
}

TEST(audio_resampled_and_mono_tracks_play_without_gaps) {
    // E: track 2 is a 1 kHz tone at 44.1 kHz, resampled to the 48 kHz mix. Q: track 2 is a mono
    // Opus tone spread over both channels. Both must reach the mix sample for sample.
    {
        AudioRig rig(syna_test::fixture("E_h264_mixed_rates.mp4"), 1);
        const int bad = discontinuities(rig.render(5.0), 1000.0, 0.25);
        CHECK_MSG(bad == 0, "44.1 kHz track: {} discontinuities", bad);
    }
    {
        AudioRig rig(syna_test::fixture("Q_h264_aac_opus_mono.mkv"), 1);
        const int bad = discontinuities(rig.render(1.8), 1000.0, 0.1);
        CHECK_MSG(bad == 0, "mono Opus track: {} discontinuities", bad);
    }
}

TEST(audio_real_recording_tracks_stay_continuous) {
    // Opt-in: SYNA_REAL_CLIP=<recording with two or more audio tracks>. Plays 90 s of the full mix
    // through the decode/FIFO/mixer path and requires that no track was padded, cut or starved.
    AudioRig rig(syna_test::real_clip(), -1);
    if (rig.pipeline->audio_stats().size() < 2) throw syna_test::Skip("recording has fewer than two audio tracks");
    const double seconds = std::min(90.0, rig.pipeline->duration_us() / 1e6 - 1.0);
    rig.render(seconds);
    for (const AudioTrackStats& s : rig.pipeline->audio_stats()) {
        std::printf("      track %d: gaps %lld us, overlaps %lld us, stretched %lld us, drift %.2f ms, underruns %lld\n", s.ordinal + 1,
                    static_cast<long long>(s.gap_us), static_cast<long long>(s.overlap_us), static_cast<long long>(s.stretched_us), s.drift_ms,
                    static_cast<long long>(s.underruns));
        CHECK_MSG(s.discontinuities == 0, "track {}: {} discontinuities", s.ordinal + 1, s.discontinuities);
        CHECK_MSG(s.underruns == 0, "track {}: {} underruns", s.ordinal + 1, s.underruns);
        CHECK_MSG(std::fabs(s.drift_ms) < 10.0, "track {}: drift {} ms", s.ordinal + 1, s.drift_ms);
    }
}
