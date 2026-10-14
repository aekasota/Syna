#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace syna {

// Interleaved float samples positioned on the media timeline (in samples at the mix rate).
// Writer: audio decode thread. Reader: audio output thread.
class SampleFifo {
public:
    void configure(int channels, int tolerance_frames) {
        std::lock_guard lock(m_);
        channels_ = channels;
        tolerance_ = tolerance_frames;
        data_.clear();
        head_ = 0;
        start_ = 0;
    }

    void clear() {
        std::lock_guard lock(m_);
        data_.clear();
        head_ = 0;
        start_ = 0;
    }

    // Appends frames that start at timeline sample `pos`. Small timestamp jitter is absorbed;
    // real gaps are filled with silence and overlaps are trimmed.
    void write(const float* src, int frames, int64_t pos) {
        if (frames <= 0) return;
        std::lock_guard lock(m_);
        if (empty_locked()) {
            data_.clear();
            head_ = 0;
            start_ = pos;
        } else {
            const int64_t end = end_locked();
            const int64_t gap = pos - end;
            if (gap > tolerance_) {
                data_.insert(data_.end(), static_cast<size_t>(gap) * channels_, 0.0f);
            } else if (gap < -tolerance_) {
                const int64_t skip = std::min<int64_t>(-gap, frames);
                src += skip * channels_;
                frames -= static_cast<int>(skip);
                if (frames <= 0) return;
            }
        }
        data_.insert(data_.end(), src, src + static_cast<size_t>(frames) * channels_);
    }

    // Reads frames for timeline range [pos, pos + frames), zero-filling uncovered parts, and
    // discards everything before pos + frames. Returns the number of frames that had data.
    int read(float* dst, int frames, int64_t pos) {
        std::lock_guard lock(m_);
        std::memset(dst, 0, sizeof(float) * static_cast<size_t>(frames) * channels_);
        if (empty_locked()) return 0;
        const int64_t avail_start = start_;
        const int64_t avail_end = end_locked();
        const int64_t a = std::max(pos, avail_start);
        const int64_t b = std::min(pos + frames, avail_end);
        int covered = 0;
        if (b > a) {
            const size_t src_off = head_ + static_cast<size_t>(a - avail_start) * channels_;
            std::memcpy(dst + (a - pos) * channels_, data_.data() + src_off, sizeof(float) * static_cast<size_t>(b - a) * channels_);
            covered = static_cast<int>(b - a);
        }
        consume_until_locked(pos + frames);
        return covered;
    }

    int64_t end_pos() const {
        std::lock_guard lock(m_);
        return empty_locked() ? INT64_MIN : end_locked();
    }

    int64_t start_pos() const {
        std::lock_guard lock(m_);
        return empty_locked() ? INT64_MIN : start_;
    }

    int64_t buffered_frames() const {
        std::lock_guard lock(m_);
        return static_cast<int64_t>((data_.size() - head_) / std::max(channels_, 1));
    }

private:
    bool empty_locked() const { return data_.size() <= head_; }
    int64_t end_locked() const { return start_ + static_cast<int64_t>((data_.size() - head_) / channels_); }

    void consume_until_locked(int64_t pos) {
        if (pos <= start_) return;
        const int64_t drop = std::min<int64_t>(pos - start_, (static_cast<int64_t>(data_.size()) - static_cast<int64_t>(head_)) / channels_);
        head_ += static_cast<size_t>(drop) * channels_;
        start_ += drop;
        if (head_ >= data_.size()) {
            data_.clear();
            head_ = 0;
            start_ = pos;
        } else if (head_ > 1 << 16 && head_ * 2 > data_.size()) {
            data_.erase(data_.begin(), data_.begin() + static_cast<ptrdiff_t>(head_));
            head_ = 0;
        }
    }

    mutable std::mutex m_;
    std::vector<float> data_;
    size_t head_ = 0;
    int64_t start_ = 0;
    int channels_ = 2;
    int tolerance_ = 0;
};

}  // namespace syna
