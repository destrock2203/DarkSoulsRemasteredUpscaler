// DSRProbe: who waits between frames. The executable's imports of Sleep / SleepEx /
// WaitForSingleObject(Ex) / QueryPerformanceCounter are hooked (import table: only the game's own
// calls) and aggregated per caller and thread. A frame limiter shows up as a caller that sleeps (or
// spins on QueryPerformanceCounter) every frame on the present thread or a thread that signals it.

#include "core/log.h"
#include "game/frame_timer.h"
#include "hooks/hook_manager.h"
#include "probe/probe.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace dsru::probe {

namespace {

enum class Api : uint8_t { Sleep, SleepEx, Wait, WaitEx, Qpc };
const char* ApiName(Api api) {
    switch (api) {
        case Api::Sleep: return "Sleep";
        case Api::SleepEx: return "SleepEx";
        case Api::Wait: return "WaitForSingleObject";
        case Api::WaitEx: return "WaitForSingleObjectEx";
        case Api::Qpc: return "QueryPerformanceCounter";
    }
    return "?";
}

struct Key {
    Api api;
    const void* caller;
    DWORD thread;
    auto operator<=>(const Key&) const = default;
};
struct Stats {
    uint64_t calls = 0;
    double ms = 0;
    DWORD lastArgument = 0;  // Sleep / wait timeout
};

std::mutex g_mutex;
std::map<Key, Stats> g_window;
double g_toMs = 0;
DWORD g_presentThread = 0;

using Sleep_t = void(WINAPI*)(DWORD);
using SleepEx_t = DWORD(WINAPI*)(DWORD, BOOL);
using Wait_t = DWORD(WINAPI*)(HANDLE, DWORD);
using WaitEx_t = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
using Qpc_t = BOOL(WINAPI*)(LARGE_INTEGER*);
Sleep_t oSleep;
SleepEx_t oSleepEx;
Wait_t oWait;
WaitEx_t oWaitEx;
Qpc_t oQpc;

double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);  // ours: not the hooked import
    return static_cast<double>(t.QuadPart) * g_toMs;
}

void Record(Api api, const void* caller, double ms, DWORD argument) {
    std::lock_guard lock(g_mutex);
    Stats& s = g_window[{api, caller, GetCurrentThreadId()}];
    ++s.calls;
    s.ms += ms;
    s.lastArgument = argument;
}

void WINAPI hkSleep(DWORD ms) {
    const void* caller = _ReturnAddress();
    const double t0 = Now();
    oSleep(ms);
    Record(Api::Sleep, caller, Now() - t0, ms);
}

DWORD WINAPI hkSleepEx(DWORD ms, BOOL alertable) {
    const void* caller = _ReturnAddress();
    const double t0 = Now();
    const DWORD r = oSleepEx(ms, alertable);
    Record(Api::SleepEx, caller, Now() - t0, ms);
    return r;
}

DWORD WINAPI hkWait(HANDLE handle, DWORD ms) {
    const void* caller = _ReturnAddress();
    const double t0 = Now();
    const DWORD r = oWait(handle, ms);
    Record(Api::Wait, caller, Now() - t0, ms);
    return r;
}

DWORD WINAPI hkWaitEx(HANDLE handle, DWORD ms, BOOL alertable) {
    const void* caller = _ReturnAddress();
    const double t0 = Now();
    const DWORD r = oWaitEx(handle, ms, alertable);
    Record(Api::WaitEx, caller, Now() - t0, ms);
    return r;
}

BOOL WINAPI hkQpc(LARGE_INTEGER* counter) {
    const void* caller = _ReturnAddress();
    Record(Api::Qpc, caller, 0.0, 0);
    const BOOL ok = oQpc(counter);
    if (ok && counter) game::frame_timer::OnQpc(caller, counter->QuadPart);
    return ok;
}

// Frame times between presents.
std::vector<double> g_frameTimes;
double g_lastPresent = 0;
double g_windowStart = 0;
uint64_t g_windowPresents = 0;

void Report(uint64_t present, double seconds) {
    std::map<Key, Stats> window;
    {
        std::lock_guard lock(g_mutex);
        window.swap(g_window);
    }
    const double frames = static_cast<double>(std::max<uint64_t>(1, g_windowPresents));
    std::sort(g_frameTimes.begin(), g_frameTimes.end());
    const double avg = g_frameTimes.empty() ? 0 : [&] {
        double sum = 0;
        for (double t : g_frameTimes) sum += t;
        return sum / static_cast<double>(g_frameTimes.size());
    }();
    const auto pct = [&](double p) {
        return g_frameTimes.empty() ? 0.0 : g_frameTimes[std::min(g_frameTimes.size() - 1,
                                                                   static_cast<size_t>(p * g_frameTimes.size()))];
    };
    log::Info("Timing at present {}: {:.1f} FPS over {:.1f} s, frame time avg {:.2f} ms (min {:.2f}, p50 {:.2f}, p99 "
              "{:.2f}, max {:.2f}), present thread {}",
              present, frames / seconds, seconds, avg, pct(0.0), pct(0.5), pct(0.99), pct(1.0), g_presentThread);

    // Per caller, per frame: the waits that happen about once a frame are the interesting ones.
    std::vector<std::pair<Key, Stats>> sorted(window.begin(), window.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        const double wa = a.first.api == Api::Qpc ? static_cast<double>(a.second.calls) * 1e-4 : a.second.ms;
        const double wb = b.first.api == Api::Qpc ? static_cast<double>(b.second.calls) * 1e-4 : b.second.ms;
        return wa > wb;
    });
    int shown = 0;
    for (const auto& [key, s] : sorted) {
        if (shown >= 14) break;
        if (key.api != Api::Qpc && s.ms < 1.0 && s.calls < frames) continue;
        ++shown;
        log::Info("  {:<23} from {:<34} thread {:>6}{} {:>7} calls ({:.2f}/frame) {:>9.1f} ms ({:.2f} ms/frame) last "
                  "argument {}",
                  ApiName(key.api), DescribeAddress(key.caller), key.thread,
                  key.thread == g_presentThread ? " [present]" : "          ", s.calls,
                  static_cast<double>(s.calls) / frames, s.ms, s.ms / frames, static_cast<int>(s.lastArgument));
    }
    g_frameTimes.clear();
}

}  // namespace

void InstallTimingHooks() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_toMs = 1000.0 / static_cast<double>(f.QuadPart);
    HMODULE exe = GetModuleHandleW(nullptr);
    int patched = 0;
    patched += hooks::PatchImport(exe, "kernel32.dll", "Sleep", &hkSleep, &oSleep);
    patched += hooks::PatchImport(exe, "kernel32.dll", "SleepEx", &hkSleepEx, &oSleepEx);
    patched += hooks::PatchImport(exe, "kernel32.dll", "WaitForSingleObject", &hkWait, &oWait);
    patched += hooks::PatchImport(exe, "kernel32.dll", "WaitForSingleObjectEx", &hkWaitEx, &oWaitEx);
    patched += hooks::PatchImport(exe, "kernel32.dll", "QueryPerformanceCounter", &hkQpc, &oQpc);
    log::Info("Timing: {} of 5 imports hooked (Sleep, SleepEx, WaitForSingleObject(Ex), QueryPerformanceCounter)", patched);
    game::frame_timer::Install(false);
}

void TimingOnPresent(uint64_t present) {
    g_presentThread = GetCurrentThreadId();
    const double now = Now();
    if (g_lastPresent > 0) g_frameTimes.push_back(now - g_lastPresent);
    g_lastPresent = now;
    if (g_windowStart == 0) g_windowStart = now;
    ++g_windowPresents;
    // Every 10 s for the first 3 minutes, then every minute.
    const double interval = present < 60 * 180 ? 10000.0 : 60000.0;
    if (now - g_windowStart >= interval) {
        Report(present, (now - g_windowStart) / 1000.0);
        g_windowStart = now;
        g_windowPresents = 0;
    }
}

}  // namespace dsru::probe
