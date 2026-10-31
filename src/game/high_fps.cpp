#include "game/high_fps.h"

#include "core/log.h"
#include "game/pattern_scan.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace dsru::game::high_fps {

namespace {

// Flipper factory, mode 5: GetFrameClock(); mov edx, 140; jmp -> SetRate(clock, edx, 0).
// GetFrameClock is `mov rax, [global]; test rax, rax; je ...; ret`.
constexpr const char* kClockPattern = "E8 ?? ?? ?? ?? BA 8C 00 00 00 EB";

// The limiter waits until (QPC - start) * rate / frequency >= ticks, then ++ticks (one per frame).
constexpr size_t kClockRate = 0x60, kClockStart = 0x678, kClockTicks = 0x690;

std::atomic<int> g_target{kGameFps};
int g_modFps = 0;
int64_t g_qpcFrequency = 0;
uintptr_t g_image = 0;
uintptr_t g_clockGlobal = 0;  // address of the frame clock pointer
bool g_available = false;
const char* g_reason = "DS1 FPS Unlock is not installed";
std::atomic<bool> g_active{false};
int g_appliedTarget = 0;  // what the clock was last set to (0: never touched)
bool g_limiterFailed = false;

// SEH: the clock can go away under us.
template <class T>
bool Read(uintptr_t address, T& out) {
    __try {
        out = *reinterpret_cast<const volatile T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Write(uintptr_t address, T value) {
    __try {
        *reinterpret_cast<volatile T*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t Clock() {
    uintptr_t clock = 0;
    return Read(g_clockGlobal, clock) ? clock : 0;
}

int ClockRate() {
    uint32_t rate = 0;
    const uintptr_t clock = Clock();
    return clock && Read(clock + kClockRate, rate) ? static_cast<int>(rate) : -1;
}

// The clock's rate for `fps` frames per second.
int EngineRate(int fps) {
    return std::max(1, static_cast<int>(std::lround(fps * static_cast<double>(kGameFps) / g_modFps)));
}

// The new rate with the schedule's phase kept: the start moves so that the ticks already counted fall
// due now at the new rate (no catch-up burst, no stall). The engine's own SetRate zeroes the start
// instead, which it only seeds again at scene changes: with a zero start the limiter never waits.
// On the present thread, where the limiter runs.
bool SetClockRate(int fps) {
    const uintptr_t clock = Clock();
    uint32_t ticks = 0;
    if (!clock || !Read(clock + kClockTicks, ticks)) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int rate = EngineRate(fps);
    const int64_t start = now.QuadPart - static_cast<int64_t>(ticks) * g_qpcFrequency / rate;
    return Write(clock + kClockRate, static_cast<uint32_t>(rate)) && Write(clock + kClockStart, start);
}

// The engine setting its own rate (scene changes): logged the first times.
void LogReapply(int rate) {
    static int count = 0;
    if (++count <= 20)
        log::Info("High FPS: the engine set its frame clock to {}, set again{}", rate,
                  count == 20 ? " (not logged any more)" : "");
}

void Apply(int target) {
    if (!SetClockRate(target)) {
        log::Warn("High FPS: the engine frame clock is not there yet");
        return;
    }
    g_active = true;
    log::Info("High FPS: limit {} FPS (clock rate {})", target, EngineRate(target));
    g_appliedTarget = target;
}

// Frames per second over one-second windows: a limiter that doesn't hold the limit (the frame clock
// isn't what this module thinks) gets the mod's own MaxFPS back rather than running unbounded.
void WatchLimiter(int target) {
    static int64_t windowStart = 0;
    static int windowFrames = 0, overWindows = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ++windowFrames;
    if (!windowStart) windowStart = now.QuadPart;
    const double seconds = static_cast<double>(now.QuadPart - windowStart) / g_qpcFrequency;
    if (seconds < 1.0) return;
    const double fps = windowFrames / seconds;
    windowStart = now.QuadPart;
    windowFrames = 0;
    overWindows = fps > target * 1.5 + 10 ? overWindows + 1 : 0;
    if (overWindows < 2) return;
    log::Error("High FPS: {:.0f} FPS with the limit at {} - the frame limiter isn't holding, back to DS1 FPS Unlock's {}",
               fps, target, g_modFps);
    g_limiterFailed = true;
    g_target = g_modFps;
    if (SetClockRate(g_modFps)) g_appliedTarget = g_modFps;
}

}  // namespace

void Install(int targetFps, int modFps) {
    if (!modFps) return;
    g_modFps = modFps;
    g_target = std::clamp(targetFps ? targetFps : modFps, kMinFps, kMaxFps);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_qpcFrequency = frequency.QuadPart;
    g_image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto clockHit = FindPattern(kClockPattern);
    const uintptr_t getClock = clockHit ? ResolveRelative(*clockHit, 1, 5) : 0;
    if (!getClock || *reinterpret_cast<const uint16_t*>(getClock) != 0x8B48 ||
        *reinterpret_cast<const uint8_t*>(getClock + 2) != 0x05) {
        log::Warn("High FPS: the engine's frame clock was not found - the limit stays DS1 FPS Unlock's MaxFPS");
        g_reason = "the game's frame clock was not found in this executable (see log)";
        return;
    }
    g_clockGlobal = ResolveRelative(getClock, 3, 7);
    g_available = true;
    g_reason = "";
    log::Info("High FPS: frame clock pointer at exe+0x{:x}, DS1 FPS Unlock MaxFPS {}, limit {} FPS",
              g_clockGlobal - g_image, modFps, g_target.load());
}

void OnFrame() {
    if (!g_available) return;
    const int target = g_target.load();
    if (target == g_modFps && !g_appliedTarget) return;  // the mod's own limit: nothing to change
    if (!Clock()) return;                               // not created yet
    // Once per target, and again if the engine set its own rate. Only then: moving the clock's start
    // every frame would release the limiter every frame.
    if (g_appliedTarget != target) {
        Apply(target);
    } else if (const int rate = ClockRate(); rate >= 0 && rate != EngineRate(target)) {
        LogReapply(rate);
        SetClockRate(target);
    }
    if (!g_limiterFailed) WatchLimiter(target);
}

void SetTarget(int fps) {
    if (g_limiterFailed || !g_available) return;
    fps = std::clamp(fps, kMinFps, kMaxFps);
    if (g_target.exchange(fps) != fps) log::Info("High FPS: limit {} FPS", fps);
}

Status GetStatus() {
    Status s;
    s.available = g_available;
    s.active = g_active.load() || (g_available && g_target.load() == g_modFps);
    s.target = g_target.load();
    s.modFps = g_modFps;
    if (!g_available)
        s.reason = g_reason;
    else if (g_limiterFailed)
        s.reason = "the frame limiter didn't hold the limit: back to DS1 FPS Unlock's MaxFPS (see log)";
    else if (!s.active)
        s.reason = "waiting for the engine's frame clock";
    return s;
}

}  // namespace dsru::game::high_fps
