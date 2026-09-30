#include "core/log.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>

namespace dsru::log {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::atomic<Level> g_level{Level::Info};

// One file per session (the previous session's is kept by bootstrap as <name>.prev.log). When it
// reaches kMaxFileSize the oldest lines are removed: the file keeps its first kKeepHead bytes (the
// session's start: version, config, GPU) and its most recent kKeepTail bytes, and writing goes on.
constexpr size_t kMaxFileSize = 10 * 1024 * 1024;
constexpr size_t kKeepHead = 32 * 1024, kKeepTail = 6 * 1024 * 1024;
std::filesystem::path g_path;
size_t g_written = 0;
size_t g_head = SIZE_MAX;  // bytes of the session's start kept, fixed by the first trim
size_t g_removed = 0;      // bytes removed so far

// A message identical to the previous one is counted instead of written again.
std::string g_last;
Level g_lastLevel = Level::Info;
size_t g_repeats = 0;

void FlushRepeats();

// Drops the lines between the session's start and its last kKeepTail bytes. g_mutex held, file open.
void TrimOldLines() {
    std::fclose(g_file);
    g_file = nullptr;
    std::string text;
    std::error_code ec;
    const auto size = std::filesystem::file_size(g_path, ec);
    if (FILE* in = _wfsopen(g_path.c_str(), L"rb", _SH_DENYNO)) {
        if (!ec) {
            text.resize(static_cast<size_t>(size));
            text.resize(std::fread(text.data(), 1, text.size(), in));
        }
        std::fclose(in);
    }
    // Whole lines: the head ends after a newline, the tail starts after one. The head is the same on
    // every trim, so the previous marker (right after it) is removed with the rest.
    if (g_head == SIZE_MAX) {
        const size_t limit = (std::min)(kKeepHead, text.size());
        g_head = limit ? text.rfind('\n', limit - 1) + 1 : 0;  // npos + 1 == 0: no complete line
    }
    const size_t head = (std::min)(g_head, text.size());
    size_t tail = head;  // nothing to remove unless the file is large
    if (text.size() > kKeepTail) {
        const size_t newline = text.find('\n', text.size() - kKeepTail);
        if (newline != std::string::npos) tail = (std::max)(newline + 1, head);
    }
    g_file = _wfsopen(g_path.c_str(), L"wb", _SH_DENYWR);
    g_written = 0;
    if (!g_file) return;
    // The previous marker is part of what goes: don't count it as log lines.
    const std::string_view dropped(text.data() + head, tail - head);
    size_t droppedLines = dropped.size();
    if (dropped.starts_with("[... ")) droppedLines -= (std::min)(dropped.find('\n') + 1, dropped.size());
    g_removed += droppedLines;
    const std::string marker = std::format("[... {:.1f} MB of older lines removed ...]\n",
                                           static_cast<double>(g_removed) / (1024.0 * 1024.0));
    std::fwrite(text.data(), 1, head, g_file);
    std::fwrite(marker.data(), 1, marker.size(), g_file);
    std::fwrite(text.data() + tail, 1, text.size() - tail, g_file);
    std::fflush(g_file);
    g_written = head + marker.size() + (text.size() - tail);
}

const char* LevelTag(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

}  // namespace

void Init(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    g_path = file;
    // Truncated on every launch: the log describes a single session.
    g_file = _wfsopen(file.c_str(), L"wb", _SH_DENYWR);
    g_written = 0;
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    FlushRepeats();
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void SetLevel(Level level) { g_level = level; }

bool Enabled(Level level) { return level >= g_level.load(std::memory_order_relaxed); }

Level ParseLevel(std::string_view text, Level fallback) {
    if (IEquals(text, "trace")) return Level::Trace;
    if (IEquals(text, "debug")) return Level::Debug;
    if (IEquals(text, "info")) return Level::Info;
    if (IEquals(text, "warn")) return Level::Warn;
    if (IEquals(text, "error")) return Level::Error;
    return fallback;
}

namespace {

// With g_mutex held.
void WriteLine(Level level, std::string_view message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string line = std::format("[{:02}:{:02}:{:02}.{:03}] [{}] [{:>5}] {}\n", t.wHour, t.wMinute,
                                         t.wSecond, t.wMilliseconds, LevelTag(level), GetCurrentThreadId(), message);
    OutputDebugStringA(line.c_str());
    if (!g_file) return;
    std::fwrite(line.data(), 1, line.size(), g_file);
    // Flush every line: the most interesting log lines are the ones right before a crash.
    std::fflush(g_file);
    g_written += line.size();
    if (g_written >= kMaxFileSize) TrimOldLines();
}

void FlushRepeats() {
    if (g_repeats) WriteLine(g_lastLevel, std::format("(previous message repeated {} more times)", g_repeats));
    g_repeats = 0;
}

}  // namespace

void Write(Level level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    if (level == g_lastLevel && message == g_last) {
        ++g_repeats;
        return;
    }
    FlushRepeats();
    g_last.assign(message);
    g_lastLevel = level;

    WriteLine(level, message);
}

}  // namespace dsru::log
