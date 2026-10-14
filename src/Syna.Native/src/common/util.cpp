#include "common/util.h"

#include <cstdlib>
#include <cstring>
#include <functional>

namespace syna {

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

int64_t qpc_now() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t qpc_frequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

void set_thread_name(const wchar_t* name) {
    SetThreadDescription(GetCurrentThread(), name);
}

namespace {
thread_local std::string t_last_error;
}

void set_last_error(std::string message) { t_last_error = std::move(message); }
const char* last_error() { return t_last_error.c_str(); }

char* dup_for_abi(const std::string& text) {
    char* p = static_cast<char*>(std::malloc(text.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, text.data(), text.size());
    p[text.size()] = '\0';
    return p;
}

PreciseWaiter::PreciseWaiter() {
    timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
    if (!timer_) timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
}

bool PreciseWaiter::wait_us(int64_t us, HANDLE wake_event) {
    if (us <= 0) {
        return wake_event && WaitForSingleObject(wake_event, 0) == WAIT_OBJECT_0;
    }
    LARGE_INTEGER due;
    due.QuadPart = -us * 10;  // relative, 100 ns units
    SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE);
    HANDLE handles[2] = {timer_.get(), wake_event};
    DWORD r = WaitForMultipleObjects(wake_event ? 2 : 1, handles, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0 + 1) {
        CancelWaitableTimer(timer_.get());
        return true;
    }
    return false;
}

}  // namespace syna
