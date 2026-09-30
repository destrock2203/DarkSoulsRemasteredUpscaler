#include "core/log.h"
#include "core/paths.h"
#include "hooks/d3d11_hooks.h"
#include "hooks/hook_manager.h"

#include <Windows.h>

// Keep DllMain minimal: we are loaded as dxgi.dll at process start, under the loader lock. The only
// work here is an import-table patch (no thread suspension) so the game's D3D11CreateDevice call is
// seen; initialization happens on the first dxgi export call or that D3D11CreateDevice.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            dsru::paths::SetSelfModule(module);
            dsru::hooks::d3d11::InstallEarlyHooks();
            break;
        case DLL_PROCESS_DETACH:
            // On process termination other threads are gone and D3D objects half-destroyed.
            if (!reserved) dsru::hooks::Shutdown();
            dsru::log::Info("Process exiting normally");
            dsru::log::Shutdown();
            break;
    }
    return TRUE;
}
