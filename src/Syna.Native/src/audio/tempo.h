#pragma once

#include <vector>

struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;

namespace syna {

// Pitch-preserving speed change through FFmpeg's atempo filter (avfilter is delay-loaded, so it is
// only mapped into the process the first time playback speed differs from 1x).
class Tempo {
public:
    Tempo() = default;
    ~Tempo();
    Tempo(const Tempo&) = delete;
    Tempo& operator=(const Tempo&) = delete;

    bool configure(double rate, int channels, int sample_rate);
    void release();
    bool active() const { return graph_ != nullptr; }

    bool push(const float* frames, int count);
    // Copies up to max_frames produced frames; returns the number copied.
    int pull(float* out, int max_frames);

private:
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* src_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    AVFrame* in_ = nullptr;
    AVFrame* out_ = nullptr;
    std::vector<float> pending_;
    size_t pending_head_ = 0;
    int channels_ = 2;
    int sample_rate_ = 48000;
    long long next_pts_ = 0;
};

}  // namespace syna
