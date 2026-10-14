#pragma once

#include "common/ffmpeg.h"
#include "common/util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace syna {

// Packets tagged with the seek generation ("serial") they belong to. A null packet marks end of stream.
struct QueuedPacket {
    AVPacket* packet = nullptr;
    int serial = 0;
    int64_t duration_us = 0;
};

class PacketQueue {
public:
    explicit PacketQueue(HANDLE consumed_event = nullptr, HANDLE pushed_event = nullptr)
        : consumed_event_(consumed_event), pushed_event_(pushed_event) {}
    ~PacketQueue() { flush(); }
    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    void push(AVPacket* packet, int serial, int64_t duration_us) {
        {
            std::lock_guard lock(m_);
            if (aborted_) {
                av_packet_free(&packet);
                return;
            }
            if (packet) bytes_ += packet->size;
            duration_us_ += duration_us;
            q_.push_back({packet, serial, duration_us});
        }
        cv_.notify_one();
        if (pushed_event_) SetEvent(pushed_event_);
    }

    bool pop(QueuedPacket& out, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_);
        if (!cv_.wait_for(lock, timeout, [&] { return aborted_ || !q_.empty(); })) return false;
        if (aborted_) return false;
        take_front(out);
        return true;
    }

    bool try_pop(QueuedPacket& out) {
        std::lock_guard lock(m_);
        if (aborted_ || q_.empty()) return false;
        take_front(out);
        return true;
    }

    void flush() {
        std::lock_guard lock(m_);
        for (auto& p : q_) av_packet_free(&p.packet);
        q_.clear();
        bytes_ = 0;
        duration_us_ = 0;
    }

    void abort() {
        {
            std::lock_guard lock(m_);
            aborted_ = true;
        }
        cv_.notify_all();
    }

    size_t count() const { std::lock_guard lock(m_); return q_.size(); }
    int64_t bytes() const { std::lock_guard lock(m_); return bytes_; }
    int64_t duration_us() const { std::lock_guard lock(m_); return duration_us_; }

private:
    void take_front(QueuedPacket& out) {
        out = q_.front();
        q_.pop_front();
        if (out.packet) bytes_ -= out.packet->size;
        duration_us_ -= out.duration_us;
        if (consumed_event_) SetEvent(consumed_event_);
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<QueuedPacket> q_;
    int64_t bytes_ = 0;
    int64_t duration_us_ = 0;
    bool aborted_ = false;
    HANDLE consumed_event_;
    HANDLE pushed_event_;
};

struct VideoFrame {
    FramePtr frame;            // null for an end-of-stream marker
    int64_t pts_us = 0;        // timeline time
    int64_t duration_us = 0;
    int serial = 0;
    bool eos = false;
};

// Decoded frames between the decode thread and the render thread. Bounded: the decoder blocks
// when full, which also bounds the number of hardware surfaces in flight.
class FrameQueue {
public:
    FrameQueue(size_t capacity, HANDLE pushed_event) : capacity_(capacity), pushed_event_(pushed_event) {}

    // Returns false if aborted or if the frame's serial became stale while waiting.
    bool push(VideoFrame&& f, const std::atomic<int>& current_serial) {
        std::unique_lock lock(m_);
        not_full_.wait(lock, [&] { return aborted_ || q_.size() < capacity_ || f.serial != current_serial.load(); });
        if (aborted_ || f.serial != current_serial.load()) return false;
        q_.push_back(std::move(f));
        lock.unlock();
        if (pushed_event_) SetEvent(pushed_event_);
        return true;
    }

    // Front frame info without removing it.
    bool peek(int64_t& pts_us, int64_t& duration_us, int& serial, bool& eos) {
        std::lock_guard lock(m_);
        if (q_.empty()) return false;
        const VideoFrame& f = q_.front();
        pts_us = f.pts_us;
        duration_us = f.duration_us;
        serial = f.serial;
        eos = f.eos;
        return true;
    }

    bool pop(VideoFrame& out) {
        {
            std::lock_guard lock(m_);
            if (q_.empty()) return false;
            out = std::move(q_.front());
            q_.pop_front();
        }
        not_full_.notify_all();
        return true;
    }

    // Drops frames whose serial differs from keep_serial.
    void drop_stale(int keep_serial) {
        {
            std::lock_guard lock(m_);
            std::erase_if(q_, [&](const VideoFrame& f) { return f.serial != keep_serial; });
        }
        not_full_.notify_all();
    }

    void wake() { not_full_.notify_all(); }

    void abort() {
        {
            std::lock_guard lock(m_);
            aborted_ = true;
        }
        not_full_.notify_all();
    }

    size_t size() const { std::lock_guard lock(m_); return q_.size(); }
    size_t capacity() const { return capacity_; }

private:
    mutable std::mutex m_;
    std::condition_variable not_full_;
    std::deque<VideoFrame> q_;
    size_t capacity_;
    bool aborted_ = false;
    HANDLE pushed_event_;
};

}  // namespace syna
