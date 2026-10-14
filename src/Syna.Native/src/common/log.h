#pragma once

#include <format>
#include <string>
#include <string_view>

// Structured, line-oriented log: "2026-10-02 12:00:00.123 INFO  [component] message key=value".
// Never call from per-frame or per-buffer paths except at Debug level.
namespace syna::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void init(const std::wstring& directory, bool verbose);
void shutdown();
bool enabled(Level level);
void write(Level level, std::string_view component, std::string_view message);

template <class... Args>
void debug(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, component, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, component, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, component, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Error)) write(Level::Error, component, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace syna::log
