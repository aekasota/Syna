#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace syna {

using Microsoft::WRL::ComPtr;

std::string to_utf8(std::wstring_view text);

int64_t qpc_now();
int64_t qpc_frequency();
inline int64_t qpc_to_us(int64_t ticks) { return ticks * 1000000 / qpc_frequency(); }
inline int64_t us_to_qpc(int64_t us) { return us * qpc_frequency() / 1000000; }
inline double qpc_to_ms(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpc_frequency()); }

void set_thread_name(const wchar_t* name);

void set_last_error(std::string message);
const char* last_error();

// malloc-allocated copy for strings handed across the C ABI (freed by syna_free_string).
char* dup_for_abi(const std::string& text);

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// Sleeps with sub-millisecond precision using a high-resolution waitable timer.
// Waking early is allowed through the optional wake event.
class PreciseWaiter {
public:
    PreciseWaiter();
    // Returns true if woken by the event, false on timeout.
    bool wait_us(int64_t us, HANDLE wake_event);

private:
    UniqueHandle timer_;
};

class ScopeExit {
public:
    template <class F>
    explicit ScopeExit(F&& f) : fn_(std::forward<F>(f)) {}
    ~ScopeExit() { if (fn_) fn_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    std::function<void()> fn_;
};

}  // namespace syna
