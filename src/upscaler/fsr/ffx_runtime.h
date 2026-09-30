#pragma once

// The FidelityFX API runtime (amd_fidelityfx_loader_dx12.dll and its providers), shared by FSR
// upscaling and FSR frame generation. Only built with the SDK (DSRU_HAS_FIDELITYFX).
#ifdef DSRU_HAS_FIDELITYFX

#include <ffx_api.h>

namespace dsru::ffx {

struct Functions {
    PfnFfxCreateContext createContext{};
    PfnFfxDestroyContext destroyContext{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
};

// Loads the loader and the providers present in the plugin folder (upscaler, frame generation),
// once, after their signature check. Thread-safe. False when the loader is unusable.
bool Load();
// Valid after Load() returned true.
const Functions& Api();

const char* ReturnCode(ffxReturnCode_t code);
// ffxApiMessage for context creation: FidelityFX's messages into our log.
void Message(uint32_t type, const wchar_t* message);

}  // namespace dsru::ffx

#endif
