#include "game/flipper.h"

#include "core/log.h"
#include "game/rtti.h"

#include <Windows.h>

#include <array>
#include <format>
#include <mutex>

namespace dsru::game::flipper {

namespace {

constexpr const char* kDecorated[] = {
    ".?AVFrpgFlipper_30FPS@NS_FRPG@@",        ".?AVFrpgFlipper_30FPS_DS1@NS_FRPG@@",
    ".?AVFrpgFlipper_30FPS_DS1_PAL@NS_FRPG@@", ".?AVFrpgFlipper_30FPS_NoSkip@NS_FRPG@@",
    ".?AVFrpgFlipper_60FPS@NS_FRPG@@",        ".?AVFrpgFlipper_60FPS_NoSkip@NS_FRPG@@",
    ".?AVFrpgFlipper_140FPS@NS_FRPG@@",
};
constexpr const char* kNames[] = {"30FPS", "30FPS_DS1", "30FPS_DS1_PAL", "30FPS_NoSkip",
                                  "60FPS", "60FPS_NoSkip", "140FPS"};
static_assert(std::size(kDecorated) == static_cast<size_t>(Mode::kCount));

std::mutex g_mutex;
bool g_initialized = false;
std::array<uintptr_t, static_cast<size_t>(Mode::kCount)> g_vtables{};
uintptr_t g_baseVTable = 0;
Instance g_current{};

Mode ModeOf(uintptr_t vtable) {
    for (size_t i = 0; i < g_vtables.size(); ++i)
        if (g_vtables[i] && g_vtables[i] == vtable) return static_cast<Mode>(i);
    return Mode::kCount;
}

bool IsFlipperVTable(uintptr_t value) {
    if (!value) return false;
    if (value == g_baseVTable) return true;
    for (uintptr_t v : g_vtables)
        if (v == value) return true;
    return false;
}

// Scans one committed region, under SEH (the game may free it meanwhile).
void ScanRegion(uintptr_t begin, size_t size, std::vector<Instance>& out) {
    __try {
        const auto* p = reinterpret_cast<const uintptr_t*>(begin);
        const size_t count = size / sizeof(uintptr_t);
        for (size_t i = 0; i < count; ++i) {
            if (!IsFlipperVTable(p[i])) continue;
            if (out.size() < 64) out.push_back({begin + i * sizeof(uintptr_t), ModeOf(p[i])});
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Replaces the vtable pointer of the object at `address` if it still is a flipper, under SEH.
bool SwapVTable(uintptr_t address, uintptr_t vtable) {
    __try {
        auto* slot = reinterpret_cast<volatile uintptr_t*>(address);
        if (!IsFlipperVTable(*slot)) return false;  // the object is gone
        *slot = vtable;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

const char* Name(Mode mode) {
    const auto i = static_cast<size_t>(mode);
    return i < std::size(kNames) ? kNames[i] : "FrpgFlipper";
}

bool Initialize() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) return g_baseVTable || g_vtables[static_cast<size_t>(Mode::k60)];
    g_initialized = true;
    for (size_t i = 0; i < std::size(kDecorated); ++i) {
        const auto found = rtti::FindVTables(kDecorated[i]);
        if (!found.empty()) g_vtables[i] = found.front();
    }
    if (const auto base = rtti::FindVTables(".?AVFrpgFlipper@NS_FRPG@@"); !base.empty()) g_baseVTable = base.front();
    const bool ok = g_vtables[static_cast<size_t>(Mode::k60)] && g_vtables[static_cast<size_t>(Mode::k140)];
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (size_t i = 0; i < g_vtables.size(); ++i)
        log::Debug("Flipper: {} vtable at exe+0x{:x}", kNames[i], g_vtables[i] ? g_vtables[i] - image : 0);
    if (!ok) log::Warn("Flipper: pacing classes not found in this executable");
    return ok;
}

std::vector<Instance> Scan() {
    std::vector<Instance> found;
    if (!Initialize()) return found;
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto address = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (address < end && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const bool writable = mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE);
        const bool guarded = mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS);
        // The executable's own .data holds static objects too (MEM_IMAGE of the exe only).
        const bool exeImage = mbi.Type == MEM_IMAGE && reinterpret_cast<uintptr_t>(mbi.AllocationBase) == image;
        if (mbi.State == MEM_COMMIT && writable && !guarded && (mbi.Type == MEM_PRIVATE || exeImage))
            ScanRegion(base, mbi.RegionSize, found);
        address = base + mbi.RegionSize;
    }
    std::lock_guard lock(g_mutex);
    g_current = found.empty() ? Instance{} : found.front();
    return found;
}

bool Current(Instance& out) {
    std::lock_guard lock(g_mutex);
    out = g_current;
    return g_current.address != 0;
}

bool SetMode(Mode mode) {
    std::lock_guard lock(g_mutex);
    const auto i = static_cast<size_t>(mode);
    if (!g_current.address || i >= g_vtables.size() || !g_vtables[i]) return false;
    if (!SwapVTable(g_current.address, g_vtables[i])) return false;
    g_current.mode = mode;
    return true;
}

std::string Describe() {
    std::lock_guard lock(g_mutex);
    const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::string text;
    for (size_t i = 0; i < g_vtables.size(); ++i)
        text += std::format("{}{}=exe+0x{:x}", text.empty() ? "" : ", ", kNames[i],
                            g_vtables[i] ? g_vtables[i] - image : 0);
    if (g_current.address)
        text += std::format("; current instance 0x{:x} ({})", g_current.address, Name(g_current.mode));
    else
        text += "; no instance found yet";
    return text;
}

}  // namespace dsru::game::flipper
