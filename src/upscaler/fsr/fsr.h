#pragma once

#include "upscaler/upscaler_types.h"

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

// AMD FSR upscaling through the FidelityFX API (amd_fidelityfx_loader_dx12.dll, which picks the
// upscaler provider: FSR 3.1.x everywhere, FSR 4 on the GPUs whose driver offers it).
// Built as no-ops without the SDK (DSRU_HAS_FIDELITYFX).
//
// Not thread-safe: apart from Available / Versions, calls come with render::upscale's lock held.
namespace dsru::fsr {

// The game's device (from the D3D12CreateDevice hook). Nothing is loaded yet.
void OnDeviceCreated(ID3D12Device* device);
// Loads the runtime and lists its upscaler versions, once. Called when FSR gets selected.
void Load();
// Loaded, and at least one upscaler version runs on this device.
bool Available();

struct Version {
    uint64_t id = 0;
    std::string name;  // e.g. "4.1.1", "3.1.5"
};
// Upscaler versions this device supports, as the runtime lists them (newest first). Fixed after Load.
const std::vector<Version>& Versions();
// Version to use by name, empty = the runtime's default (newest). Takes effect at the next evaluation.
void SetVersion(const std::string& name);
// The version the current context runs, empty before the first evaluation.
std::string ActiveVersion();

// Inputs in PIXEL | NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS (upscaler::Evaluate
// guarantees it). Returns false on error (logged). Command list state is NOT restored here.
bool Evaluate(const UpscaleInputs& in);

// Once per present: destroys contexts retired a few frames ago (GPU done with them).
void OnPresent();

}  // namespace dsru::fsr
