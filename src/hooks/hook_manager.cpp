#include "hooks/hook_manager.h"

#include "core/log.h"

#include <MinHook.h>
#include <Windows.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dsru::hooks {

namespace {

struct VTablePatch {
    void** slot;
    void* original;
};

std::mutex g_mutex;
bool g_initialized = false;
int g_batchDepth = 0;
bool g_batchPending = false;
std::unordered_set<void*> g_targets;
std::unordered_map<void*, void*> g_inlineTargets;  // detour -> target
std::vector<VTablePatch> g_vtablePatches;
std::unordered_map<void*, void*> g_vtableOriginals;  // detour -> original

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

bool Initialize() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) return true;
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        log::Error("MH_Initialize failed: {}", MH_StatusToString(status));
        return false;
    }
    g_initialized = true;
    return true;
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return;
    for (const VTablePatch& patch : g_vtablePatches) WriteSlot(patch.slot, patch.original);
    g_vtablePatches.clear();
    g_vtableOriginals.clear();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_targets.clear();
    g_inlineTargets.clear();
    g_initialized = false;
}

bool InstallRaw(void* target, void* detour, void** original, const char* name) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized || !target) return false;
    if (g_targets.contains(target)) {
        log::Trace("Hook {} already installed at {}", name, target);
        return true;
    }
    // A detour has a single `original`. Reusing it for a second target would overwrite that
    // original and route calls meant for the first target into the second one's code.
    if (const auto it = g_inlineTargets.find(detour); it != g_inlineTargets.end()) {
        log::Error("Hook {}: detour already used for {}, refusing to hook {}", name, it->second, target);
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        log::Error("MH_CreateHook({}) failed: {}", name, MH_StatusToString(status));
        return false;
    }
    status = g_batchDepth ? MH_QueueEnableHook(target) : MH_EnableHook(target);
    if (status != MH_OK) {
        log::Error("MH_EnableHook({}) failed: {}", name, MH_StatusToString(status));
        MH_RemoveHook(target);
        return false;
    }
    g_batchPending |= g_batchDepth > 0;

    g_targets.insert(target);
    g_inlineTargets[detour] = target;
    log::Debug("Hooked {} at {}", name, target);
    return true;
}

bool InstallVTableRaw(void* object, size_t index, void* detour, void** original, const char* name) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized || !object) return false;
    void** slot = &(*static_cast<void***>(object))[index];
    void* current = *slot;
    if (current == detour) {
        log::Trace("VTable hook {} already installed at {}", name, static_cast<void*>(slot));
        return true;
    }
    // One detour has one `original`: a second class with a different implementation can't
    // share it. (Not expected for the swapchain/queue classes, but calling the wrong original
    // would be fatal.)
    if (const auto it = g_vtableOriginals.find(detour); it != g_vtableOriginals.end() && it->second != current) {
        log::Warn("VTable hook {}: second vtable {} with a different implementation, not hooked", name,
                  static_cast<void*>(slot));
        return false;
    }

    *original = current;
    if (!WriteSlot(slot, detour)) {
        log::Error("VTable hook {}: VirtualProtect failed ({})", name, GetLastError());
        return false;
    }
    g_vtableOriginals[detour] = current;
    g_vtablePatches.push_back({slot, current});
    log::Debug("Hooked {} (vtable slot {}, was {})", name, static_cast<void*>(slot), current);
    return true;
}

bool PatchImportRaw(void* module, const char* dll, const char* function, void* detour, void** original) {
    auto* base = static_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool patched = false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0 || !desc->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            if (!*original) *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            patched = true;
        }
    }
    return patched;
}

bool PatchImportAddressRaw(void* module, const char* dll, void* current, void* detour, void** original) {
    auto* base = static_cast<uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !current) return false;
    bool patched = false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
        for (auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk); slot->u1.Function; ++slot) {
            if (reinterpret_cast<void*>(slot->u1.Function) != current) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            *original = current;
            slot->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
            VirtualProtect(&slot->u1.Function, sizeof(void*), old, &old);
            patched = true;
        }
    }
    return patched;
}

Batch::Batch() {
    std::lock_guard lock(g_mutex);
    ++g_batchDepth;
}

Batch::~Batch() {
    std::lock_guard lock(g_mutex);
    if (--g_batchDepth > 0 || !g_batchPending) return;
    g_batchPending = false;
    if (const MH_STATUS status = MH_ApplyQueued(); status != MH_OK)
        log::Error("MH_ApplyQueued failed: {}", MH_StatusToString(status));
}

}  // namespace dsru::hooks
