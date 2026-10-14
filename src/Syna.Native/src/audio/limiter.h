#pragma once

#include <cmath>

namespace syna {

// Peak limiter used by the playback mixer and by the export mixdown: instant attack, so the
// output never exceeds the threshold, and a smooth release.
class PeakLimiter {
public:
    static constexpr float kThreshold = 0.966f;  // -0.3 dBFS

    explicit PeakLimiter(int sample_rate = 48000)
        : release_(static_cast<float>(1.0 - std::exp(-1.0 / (kReleaseSeconds * sample_rate)))) {}

    // Gain for one frame whose loudest sample (after every other gain) is `peak`.
    float gain(float peak) {
        const float desired = peak > kThreshold ? kThreshold / peak : 1.0f;
        env_ = desired < env_ ? desired : env_ + (desired - env_) * release_;
        return env_;
    }

private:
    static constexpr double kReleaseSeconds = 0.08;
    float release_;
    float env_ = 1.0f;
};

}  // namespace syna
