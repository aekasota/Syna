#include "common/log.h"

#include "common/ffmpeg.h"
#include "common/util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace syna::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
Level g_min_level = Level::Info;

constexpr const char* level_name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?";
}

void prune_old_logs(const std::filesystem::path& dir) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> logs;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().filename().wstring().starts_with(L"native-")) logs.push_back(e);
    }
    if (logs.size() <= 10) return;
    std::sort(logs.begin(), logs.end(), [](auto& a, auto& b) { return a.last_write_time() < b.last_write_time(); });
    for (size_t i = 0; i + 10 < logs.size(); ++i) std::filesystem::remove(logs[i].path(), ec);
}

// FFmpeg messages are bridged at warning level and above, with repeated lines throttled so a
// corrupt file cannot flood the log.
std::unordered_map<std::string, int> g_ff_repeats;

void ffmpeg_log_callback(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING && !enabled(Level::Debug)) return;
    if (level > AV_LOG_VERBOSE) return;
    char line[1024];
    int print_prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &print_prefix);
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) return;
    {
        std::lock_guard lock(g_mutex);
        int& n = g_ff_repeats[text];
        if (++n > 5) {
            if (n == 6) {
                // fall through once to note suppression
                text += " (further repeats suppressed)";
            } else {
                return;
            }
        }
        if (g_ff_repeats.size() > 512) g_ff_repeats.clear();
    }
    Level l = level <= AV_LOG_ERROR ? Level::Error : level <= AV_LOG_WARNING ? Level::Warn : Level::Debug;
    write(l, "ffmpeg", text);
}

}  // namespace

void init(const std::wstring& directory, bool verbose) {
    std::lock_guard lock(g_mutex);
    g_min_level = verbose ? Level::Debug : Level::Info;
    if (g_file || directory.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    prune_old_logs(directory);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[96];
    swprintf_s(name, L"native-%04u%02u%02u-%02u%02u%02u-%lu.log", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, GetCurrentProcessId());
    auto path = std::filesystem::path(directory) / name;
    g_file = _wfsopen(path.c_str(), L"a", _SH_DENYWR);
    av_log_set_callback(ffmpeg_log_callback);
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

bool enabled(Level level) { return level >= g_min_level; }

void write(Level level, std::string_view component, std::string_view message) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u %s [", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level_name(level));
    std::string line;
    line.reserve(64 + component.size() + message.size());
    line += prefix;
    line += component;
    line += "] ";
    line += message;
    line += '\n';
#ifndef NDEBUG
    OutputDebugStringA(line.c_str());
#endif
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    fwrite(line.data(), 1, line.size(), g_file);
    if (level >= Level::Warn) fflush(g_file);
}

}  // namespace syna::log
