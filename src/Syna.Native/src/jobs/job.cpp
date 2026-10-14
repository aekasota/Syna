#include "jobs/job.h"

#include "common/log.h"
#include "common/util.h"

namespace syna {

Job::Job(Work work, bool background_priority) {
    thread_ = std::thread([this, work = std::move(work), background_priority] {
        set_thread_name(L"Syna job");
        // Background mode lowers CPU, I/O and memory priority so playback is never starved.
        if (background_priority) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        syna_result r = SYNA_E_FAIL;
        try {
            r = work(*this);
        } catch (const std::exception& e) {
            log::error("job", "exception: {}", e.what());
            r = SYNA_E_FAIL;
        } catch (...) {
            r = SYNA_E_FAIL;
        }
        if (background_priority) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        error_.store(r);
        if (r == SYNA_OK) {
            progress_.store(1.0);
            state_.store(SYNA_JOB_SUCCEEDED);
        } else {
            state_.store(r == SYNA_E_CANCELLED || cancelled_.load() ? SYNA_JOB_CANCELLED : SYNA_JOB_FAILED);
        }
    });
}

Job::~Job() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

syna_job_status Job::status() const {
    syna_job_status s;
    s.state = state_.load();
    s.error = error_.load();
    s.progress = progress_.load();
    return s;
}

void Job::set_result_json(std::string json) {
    std::lock_guard lock(result_mutex_);
    result_ = std::move(json);
}

std::string Job::result_json() const {
    std::lock_guard lock(result_mutex_);
    return result_;
}

}  // namespace syna
