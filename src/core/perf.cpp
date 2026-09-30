#include "core/perf.h"

#include <Windows.h>

#include <mutex>

namespace dsru::perf {

namespace {

std::mutex g_mutex;
double g_toMs = 0;
LARGE_INTEGER g_entered{}, g_native{}, g_lastReturn{};
Stats g_stats;


float Smooth(float average, double sample) {
    return average == 0 ? static_cast<float>(sample) : average * 0.95f + static_cast<float>(sample) * 0.05f;
}

}  // namespace

void PresentEntered() {
    std::lock_guard lock(g_mutex);
    if (g_toMs == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_toMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    QueryPerformanceCounter(&g_entered);
    if (g_lastReturn.QuadPart) g_stats.gameMs = Smooth(g_stats.gameMs, (g_entered.QuadPart - g_lastReturn.QuadPart) * g_toMs);
}

void NativePresentCalled() {
    std::lock_guard lock(g_mutex);
    QueryPerformanceCounter(&g_native);
    g_stats.modMs = Smooth(g_stats.modMs, (g_native.QuadPart - g_entered.QuadPart) * g_toMs);
}

void PresentReturned() {
    std::lock_guard lock(g_mutex);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_stats.waitMs = Smooth(g_stats.waitMs, (now.QuadPart - g_native.QuadPart) * g_toMs);
    if (g_lastReturn.QuadPart) g_stats.frameMs = Smooth(g_stats.frameMs, (now.QuadPart - g_lastReturn.QuadPart) * g_toMs);
    g_lastReturn = now;
}

Stats Get() {
    std::lock_guard lock(g_mutex);
    return g_stats;
}

}  // namespace dsru::perf
