#include "game/frame_timer.h"

#include "core/log.h"
#include "game/pattern_scan.h"
#include "hooks/hook_manager.h"

#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <thread>

namespace dsru::game::frame_timer {

namespace {

// After the wait: QueryPerformanceCounter(&t); rax = t; rbx = [rsp+58]; timer+0x688 = rax.
//     48 8D 4C 24 50        lea rcx, [rsp+50h]
//     FF 15 ?? ?? ?? ??     call [QueryPerformanceCounter]
//     48 8B 44 24 50        mov rax, [rsp+50h]          <- return address
//     48 8B 5C 24 58        mov rbx, [rsp+58h]
//     48 89 87 88 06 00 00  mov [rdi+688h], rax
constexpr const char* kSitePattern = "48 8D 4C 24 50 FF 15 ?? ?? ?? ?? 48 8B 44 24 50 48 8B 5C 24 58 48 89 87 88 06 00 00";
constexpr size_t kSiteOffset = 11;
constexpr size_t kStampOffset = 0x688;
constexpr size_t kRateOffset = 0x60;
constexpr uint32_t kGameRate = 60;

uintptr_t g_site = 0;
std::atomic<int64_t> g_lastStamp{0};
std::atomic<bool> g_scanning{false};
std::atomic<uintptr_t> g_timer{0};
std::atomic<uint32_t> g_baseRate{kGameRate};  // the rate field's value for 60 FPS
int64_t g_frequency = 0;
uint64_t g_presents = 0;
uint64_t g_nextScan = 0;

using Qpc_t = BOOL(WINAPI*)(LARGE_INTEGER*);
Qpc_t oQpc = nullptr;

BOOL WINAPI hkQpc(LARGE_INTEGER* counter) {
    const BOOL ok = oQpc(counter);
    if (ok && counter) OnQpc(_ReturnAddress(), counter->QuadPart);
    return ok;
}

bool ReadU32(uintptr_t address, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteU32(uintptr_t address, uint32_t value) {
    __try {
        *reinterpret_cast<volatile uint32_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Candidate {
    uintptr_t object;
    uint32_t rate;
    uint32_t counter;
};

// The object's timestamp history: {stamp, counter} records from timer+0x688 (latest), shifted by
// memmove before each store, about a frame apart. Strong enough a signature to
// find it without trusting any other field.
void ScanRegion(uintptr_t begin, size_t size, int64_t low, int64_t high, int64_t frame, Candidate* out, int& count) {
    __try {
        const auto* p = reinterpret_cast<const int64_t*>(begin);
        const size_t n = size / sizeof(int64_t);
        for (size_t i = 0; i + 4 < n && count < 16; ++i) {
            if (p[i] < low || p[i] > high) continue;
            // 16-byte records {stamp, counter + flag}: previous stamps at +0x698, +0x6a8.
            const int64_t previous = p[i + 2], older = p[i + 4];
            const int64_t d1 = p[i] - previous, d2 = previous - older;
            if (d1 < frame / 4 || d1 > frame * 8 || d2 < frame / 4 || d2 > frame * 8) continue;
            const uintptr_t object = begin + i * sizeof(int64_t) - kStampOffset;
            if (object < begin) continue;
            out[count++] = {object, *reinterpret_cast<const uint32_t*>(object + kRateOffset),
                            *reinterpret_cast<const uint32_t*>(object + kStampOffset + 8)};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Diagnostics when nothing matched: the first raw matches of the stamp with the memory around them.
void LogRawMatches(int64_t low, int64_t high) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto address = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    int logged = 0;
    while (address < end && logged < 12 && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const size_t n = mbi.RegionSize / 8;
            for (size_t i = 4; i + 6 < n && logged < 12; ++i) {
                int64_t q[10]{};
                if (!ReadU32(base + i * 8, *reinterpret_cast<uint32_t*>(&q[0]))) break;
                bool ok = true;
                for (int k = 0; k < 10 && ok; ++k) ok = ReadU32(base + (i - 4 + k) * 8, *reinterpret_cast<uint32_t*>(&q[k])) &&
                                                        ReadU32(base + (i - 4 + k) * 8 + 4, reinterpret_cast<uint32_t*>(&q[k])[1]);
                if (!ok || q[4] < low || q[4] > high) continue;
                ++logged;
                log::Info("Frame timer: stamp at 0x{:x} ({}): -32: {:x} {:x} {:x} {:x} | {:x} | {:x} {:x} {:x} {:x} {:x}",
                          base + i * 8, mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private",
                          q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9]);
            }
        }
        address = base + mbi.RegionSize;
    }
}

void Scan() {
    const int64_t stamp = g_lastStamp.load();
    const int64_t low = stamp - 2 * g_frequency, high = stamp + 2 * g_frequency;
    const int64_t frame = g_frequency / 60;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto address = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    const DWORD t0 = GetTickCount();
    Candidate candidates[16];
    int count = 0;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (address < end && count < 16 && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && (mbi.Protect & kWritable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            ScanRegion(base, mbi.RegionSize, low, high, frame, candidates, count);
        address = base + mbi.RegionSize;
    }
    for (int i = 0; i < count; ++i) {
        const Candidate& c = candidates[i];
        uint32_t fields[8]{};
        for (int f = 0; f < 8; ++f) ReadU32(c.object + 0x50 + f * 4, fields[f]);
        log::Info("Frame timer: candidate 0x{:x}: +0x60 = {}, counter {}; +0x50..0x6f: {} {} {} {} {} {} {} {}", c.object,
                  c.rate, c.counter, fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]);
        // The rate the engine runs at is 60 FPS: the field is 60 in some unit (kept as the base).
        if (!g_timer && c.rate > 0 && c.rate <= 1000000 && c.rate % 60 == 0) {
            g_baseRate = c.rate;
            g_timer = c.object;
        }
    }
    if (g_timer)
        log::Info("Frame timer: engine frame clock at 0x{:x} (rate field {} = 60 FPS), found in {} ms", g_timer.load(),
                  g_baseRate.load(), GetTickCount() - t0);
    else {
        log::Warn("Frame timer: engine frame clock not found ({} candidates, {} ms) - retrying later", count,
                  GetTickCount() - t0);
        static int diagnostics = 0;
        if (diagnostics++ < 2) LogRawMatches(low, high);
    }
}

}  // namespace

void Install(bool hookImport) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_frequency = f.QuadPart;
    const auto hit = FindPattern(kSitePattern);
    if (!hit) {
        log::Warn("Frame timer: the engine's frame limiter was not found in this executable - FPS unlock unavailable");
        return;
    }
    g_site = *hit + kSiteOffset;
    log::Info("Frame timer: frame limiter at exe+0x{:x}", g_site - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    if (hookImport)
        hooks::PatchImport(GetModuleHandleW(nullptr), "kernel32.dll", "QueryPerformanceCounter", &hkQpc, &oQpc);
}

void OnQpc(const void* returnAddress, int64_t value) {
    if (g_site && reinterpret_cast<uintptr_t>(returnAddress) == g_site) g_lastStamp.store(value, std::memory_order_relaxed);
}

void OnPresent() {
    ++g_presents;
    if (g_timer || !g_lastStamp.load() || g_presents < g_nextScan || g_scanning.exchange(true)) return;
    g_nextScan = g_presents + 600;
    std::thread([] {
        Scan();
        g_scanning = false;
    }).detach();
}

bool Found() { return g_timer.load() != 0; }

int Rate() {
    uint32_t rate = 0;
    const uintptr_t timer = g_timer.load();
    if (!timer || !ReadU32(timer + kRateOffset, rate)) return -1;
    return static_cast<int>(static_cast<uint64_t>(rate) * 60 / g_baseRate.load());
}

bool SetRate(int fps) {
    const uintptr_t timer = g_timer.load();
    if (!timer || fps < 0) return false;
    uint32_t current = 0;
    // Still the object: its rate is one this module (or the game) wrote.
    if (!ReadU32(timer + kRateOffset, current) || current > 1000u * g_baseRate.load()) {
        g_timer = 0;
        return false;
    }
    const auto value = static_cast<uint32_t>(static_cast<uint64_t>(fps) * g_baseRate.load() / 60);
    if (!WriteU32(timer + kRateOffset, value)) return false;
    log::Info("Frame timer: engine frame clock {} -> {} (field {})", Rate() >= 0 ? current * 60 / g_baseRate.load() : 0u,
              fps ? std::to_string(fps) + " FPS" : std::string("unlimited"), value);
    return true;
}

}  // namespace dsru::game::frame_timer
