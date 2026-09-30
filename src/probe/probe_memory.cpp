// DSRProbe: memory tools.
//  - Flipper (game/flipper.h): the live frame pacing object is looked for once the game runs, its
//    first bytes logged twice (to see which fields change per frame); F11 switches it between the
//    60 FPS and 140 FPS pacing classes, and the timing report then shows the resulting frame rate.
//  - F9 data snapshots: three presses (before / after changing a setting / after changing it back);
//    the executable's writable data that followed the setting (A == C != B) is logged.

#include "core/log.h"
#include "game/flipper.h"
#include "game/frame_timer.h"
#include "probe/probe.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dsru::probe {

namespace {

namespace flipper = game::flipper;

std::atomic<bool> g_scanning{false};
uint64_t g_measureStart = 0;  // F11 measurement: present and time of the last switch
LARGE_INTEGER g_measureTime{};
std::atomic<bool> g_scanned{false};
uint64_t g_dumpAgainAt = 0;

// Copies memory that may be gone, under SEH.
bool SafeRead(uintptr_t address, void* out, size_t size) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string HexDump(uintptr_t address, size_t size) {
    std::vector<uint32_t> words((size + 3) / 4);
    if (!SafeRead(address, words.data(), words.size() * 4)) return " <unreadable>";
    std::string out;
    for (size_t i = 0; i < words.size(); i += 4) {
        out += std::format("\n    +{:03x}:", i * 4);
        for (size_t j = i; j < i + 4 && j < words.size(); ++j) out += std::format(" {:08x}", words[j]);
        out += "   floats:";
        for (size_t j = i; j < i + 4 && j < words.size(); ++j) {
            float f;
            std::memcpy(&f, &words[j], 4);
            out += std::format(" {:.5g}", f);
        }
    }
    return out;
}

void ScanFlipper(const char* reason) {
    if (g_scanning.exchange(true)) return;
    std::thread([reason] {
        const DWORD t0 = GetTickCount();
        const auto found = flipper::Scan();
        log::Info("Flipper scan ({}): {} object(s) in {} ms", reason, found.size(), GetTickCount() - t0);
        for (const auto& instance : found)
            log::Info("  0x{:x} {}{}", instance.address, flipper::Name(instance.mode), HexDump(instance.address, 0x80));
        if (found.size() > 1) log::Info("  Several flipper objects: the first one is used by the F11 test");
        g_scanned = true;
        g_scanning = false;
    }).detach();
}

// --- Data snapshots ---

struct Region {
    uintptr_t base;
    std::vector<uint8_t> a, b, c;
};
std::vector<Region> g_regions;
int g_snapshotStep = 0;

void Snapshot(std::vector<uint8_t> Region::*member) {
    if (g_regions.empty()) {
        const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE) || (section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            Region& r = g_regions.emplace_back();
            r.base = reinterpret_cast<uintptr_t>(base) + section->VirtualAddress;
            r.a.resize(section->Misc.VirtualSize);
            r.b.resize(section->Misc.VirtualSize);
            r.c.resize(section->Misc.VirtualSize);
        }
    }
    for (Region& r : g_regions) std::memcpy((r.*member).data(), reinterpret_cast<const void*>(r.base), (r.*member).size());
}

void ReportSnapshots() {
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    int shown = 0, total = 0;
    for (const Region& r : g_regions) {
        for (size_t i = 0; i + 4 <= r.a.size(); i += 4) {
            uint32_t a, b, c;
            std::memcpy(&a, &r.a[i], 4);
            std::memcpy(&b, &r.b[i], 4);
            std::memcpy(&c, &r.c[i], 4);
            if (a != c || a == b) continue;
            ++total;
            if (shown++ < 300) {
                float fa, fb;
                std::memcpy(&fa, &a, 4);
                std::memcpy(&fb, &b, 4);
                log::Info("  exe+0x{:x}: {:08x} -> {:08x} -> back   (int {} -> {}, float {:.5g} -> {:.5g})",
                          r.base + i - image, a, b, static_cast<int32_t>(a), static_cast<int32_t>(b), fa, fb);
            }
        }
    }
    log::Info("Data snapshots: {} dword(s) followed the change{}", total, total > 300 ? " (first 300 shown)" : "");
}

}  // namespace

void MemoryOnPresent(uint64_t present, bool flipperKey, bool snapshotKey) {
    // The pacing object exists once the game loop runs; look a bit after startup.
    if (present == 900) ScanFlipper("startup");
    if (g_scanned && !g_dumpAgainAt) g_dumpAgainAt = present + 120;
    if (g_dumpAgainAt && present == g_dumpAgainAt) {
        flipper::Instance current;
        if (flipper::Current(current))
            log::Info("Flipper 0x{:x} ({}) 120 frames later:{}", current.address, flipper::Name(current.mode),
                      HexDump(current.address, 0x80));
    }

    // F11: the engine's frame clock (game/frame_timer.h), 60 <-> unlimited, measured over 5 s.
    game::frame_timer::OnPresent();
    if (g_measureStart) {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double seconds =
            static_cast<double>(now.QuadPart - g_measureTime.QuadPart) / static_cast<double>(frequency.QuadPart);
        if (seconds >= 5.0) {
            log::Info("F11 measurement: {:.1f} FPS over {:.1f} s with the engine frame clock at {} (0 = unlimited)",
                      static_cast<double>(present - g_measureStart) / seconds, seconds, game::frame_timer::Rate());
            g_measureStart = present;
            g_measureTime = now;
        }
    }
    if (flipperKey) {
        if (!game::frame_timer::Found()) {
            log::Info("F11: the engine frame clock is not found yet (looked for once the game runs)");
        } else {
            const int next = game::frame_timer::Rate() == 0 ? 60 : 0;
            if (game::frame_timer::SetRate(next)) {
                log::Info("F11: engine frame clock set to {} at present {} - watch whether the game itself runs faster "
                          "(animations, timers)",
                          next ? "60" : "unlimited", present);
                g_measureStart = present;
                QueryPerformanceCounter(&g_measureTime);
            }
        }
    }

    if (snapshotKey) {
        switch (g_snapshotStep) {
            case 0:
                Snapshot(&Region::a);
                log::Info("F9: snapshot A taken. Change ONE setting in the game's options, then press F9 again.");
                break;
            case 1:
                Snapshot(&Region::b);
                log::Info("F9: snapshot B taken. Change the setting BACK, then press F9 again.");
                break;
            default:
                Snapshot(&Region::c);
                ReportSnapshots();
                break;
        }
        g_snapshotStep = (g_snapshotStep + 1) % 3;
    }
}

}  // namespace dsru::probe
