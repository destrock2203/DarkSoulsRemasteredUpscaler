#pragma once

#include <cstddef>

namespace dsru::hooks {

bool Initialize();
void Shutdown();

// Detours `target`. Installing the same target twice is a no-op that returns true, which matters
// for COM vtable hooks: every instance of a class shares the same method implementation.
bool InstallRaw(void* target, void* detour, void** original, const char* name);

template <class Fn>
bool Install(void* target, Fn detour, Fn* original, const char* name) {
    return InstallRaw(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}

// Replaces the `index`-th vtable slot of `object` instead of patching code. Use this for methods
// overlays also hook (Present, ExecuteCommandLists): tools like RTSS re-patch the function
// prologue after us without chaining to our detour, but calls still go through the vtable
// first. The slot is shared by every object of the class; installing it twice is a no-op.
bool InstallVTableRaw(void* object, size_t index, void* detour, void** original, const char* name);

template <class Fn>
bool InstallVTable(void* object, size_t index, Fn detour, Fn* original, const char* name) {
    return InstallVTableRaw(object, index, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original),
                            name);
}

// Groups the Install() calls made while it is alive so that they are enabled together. MinHook
// suspends every thread of the process (and snapshots every thread on the system) per enable,
// ~30 ms each, so hooking 20 methods one by one stalls the game for most of a second.
class Batch {
public:
    Batch();
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
};

// Import-table hook: replaces `function` imported from `dll` by `module` (every descriptor for
// that DLL). No code patching and no thread suspension, so it is usable from DllMain. Only calls
// the module makes through its import table are caught.
bool PatchImportRaw(void* module, const char* dll, const char* function, void* detour, void** original);

template <class Fn>
bool PatchImport(void* module, const char* dll, const char* function, Fn detour, Fn* original) {
    return PatchImportRaw(module, dll, function, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

// The same for imports by ordinal: replaces every import slot of `module` from `dll` whose current
// value is `current`.
bool PatchImportAddressRaw(void* module, const char* dll, void* current, void* detour, void** original);

// Address of the `index`-th virtual method of a COM object.
inline void* VTableEntry(void* object, size_t index) { return (*static_cast<void***>(object))[index]; }

}  // namespace dsru::hooks
