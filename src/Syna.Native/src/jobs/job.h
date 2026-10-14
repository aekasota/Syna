#pragma once

#include "syna_api.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace syna {

// A unit of background work on its own thread with progress, cancellation and a JSON result.
// The owner (C ABI handle) destroys it, which cancels and joins, so the worker never outlives it.
class Job {
public:
    using Work = std::function<syna_result(Job&)>;

    Job(Work work, bool background_priority);
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    void cancel() { cancelled_.store(true); }
    bool cancelled() const { return cancelled_.load(); }
    void set_progress(double p) { progress_.store(p < 0 ? 0 : p > 1 ? 1 : p); }

    syna_job_status status() const;
    void set_result_json(std::string json);
    std::string result_json() const;

private:
    std::atomic<bool> cancelled_{false};
    std::atomic<int> state_{SYNA_JOB_RUNNING};
    std::atomic<int> error_{SYNA_OK};
    std::atomic<double> progress_{0};
    mutable std::mutex result_mutex_;
    std::string result_;
    std::thread thread_;
};

}  // namespace syna
