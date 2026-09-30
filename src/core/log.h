#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace dsru::log {

enum class Level { Trace, Debug, Info, Warn, Error };

void Init(const std::filesystem::path& file);
void Shutdown();

void SetLevel(Level level);
bool Enabled(Level level);
Level ParseLevel(std::string_view text, Level fallback);

void Write(Level level, std::string_view message);

template <class... Args>
void Trace(std::format_string<Args...> fmt, Args&&... args) {
    if (Enabled(Level::Trace)) Write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void Debug(std::format_string<Args...> fmt, Args&&... args) {
    if (Enabled(Level::Debug)) Write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
    if (Enabled(Level::Info)) Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
    if (Enabled(Level::Warn)) Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
    if (Enabled(Level::Error)) Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace dsru::log
