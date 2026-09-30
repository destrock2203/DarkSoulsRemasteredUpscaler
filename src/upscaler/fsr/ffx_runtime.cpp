#include "upscaler/fsr/ffx_runtime.h"

#ifdef DSRU_HAS_FIDELITYFX

#include "core/log.h"
#include "core/paths.h"
#include "upscaler/signature.h"

#include <Windows.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace dsru::ffx {

namespace {

Functions g_api;
bool g_loaded = false;
std::once_flag g_once;

template <class Fn>
bool Resolve(HMODULE module, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (!out) log::Error("FidelityFX: amd_fidelityfx_loader_dx12.dll has no export '{}'", name);
    return out != nullptr;
}

void DoLoad() {
    // The loader loads its providers by bare name (LoadLibraryA): loading them from our folder first
    // makes it find these instead of searching the game folder / PATH. A missing provider only
    // disables its effect.
    for (const wchar_t* provider : {L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_framegeneration_dx12.dll"})
        if (std::filesystem::exists(paths::ModuleDirectory() / "DSRUpscaler" / provider))
            upscaler::LoadSignedModule("FidelityFX", provider);
    auto* module = static_cast<HMODULE>(upscaler::LoadSignedModule("FidelityFX", L"amd_fidelityfx_loader_dx12.dll"));
    if (!module) return;
    Functions api;
    if (!Resolve(module, "ffxCreateContext", api.createContext) ||
        !Resolve(module, "ffxDestroyContext", api.destroyContext) ||
        !Resolve(module, "ffxConfigure", api.configure) || !Resolve(module, "ffxQuery", api.query) ||
        !Resolve(module, "ffxDispatch", api.dispatch))
        return;
    g_api = api;
    g_loaded = true;
}

std::string Narrow(const wchar_t* text) {
    std::string out;
    for (; text && *text; ++text) out.push_back(*text < 0x80 ? static_cast<char>(*text) : '?');
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace

bool Load() {
    std::call_once(g_once, DoLoad);
    return g_loaded;
}

const Functions& Api() { return g_api; }

const char* ReturnCode(ffxReturnCode_t code) {
    switch (code) {
        case FFX_API_RETURN_OK: return "ok";
        case FFX_API_RETURN_ERROR: return "error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown descriptor type";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
        case FFX_API_RETURN_NO_PROVIDER: return "no provider";
        case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER: return "invalid parameter";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "provider too old";
        default: return "?";
    }
}

void Message(uint32_t type, const wchar_t* message) {
    if (type == FFX_API_MESSAGE_TYPE_ERROR)
        log::Error("[FidelityFX] {}", Narrow(message));
    else
        log::Warn("[FidelityFX] {}", Narrow(message));
}

}  // namespace dsru::ffx

#endif
