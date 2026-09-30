#pragma once

#include <d3d12.h>

#include <cstdint>

// DLSS 5 (3D-guided neural rendering): NVIDIA's neural relighting of the rendered frame, driven
// directly through NGX (feature 18, nvngx_dlssnr.dll), since Streamline's sl.dlss_nr isn't public.
// The runtime is not distributed with the mod: the user places nvngx_dlssnr*.dll builds in
// DARK SOULS REMASTERED/DSRUpscaler/ (e.g. NVIDIA's RTX 50 one and an RTX 40 repack side by side), and the
// one whose CUDA kernels run on this GPU is loaded. Its NGX calls go
// through dsru_nvngx.dll (upscaler/dlss_nr/nvngx_forwarder.cpp): the runtime only serves callers
// whose module path contains "nvngx.dll". RTX 50 series only.
//
// The parameters (DLSSNR.*) are not documented by NVIDIA; they are the ones community tools use
// (reference: github.com/Dagherbou/OptiScaler_DLSSNR). Options are latched when the feature is
// created: a change rebuilds it once it has held for a moment.
namespace dsru::upscaler::dlss_nr {

struct Options {
    float intensity = 1.0f;
    float localTone = 0.5f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;
    uint32_t style = 1;  // model / look
};

struct Inputs {
    ID3D12Device* device = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* color = nullptr;          // NON_PIXEL_SHADER_RESOURCE (or readable) state
    uint32_t width = 0, height = 0;           // color / output size
    ID3D12Resource* depth = nullptr;          // shader-readable state, motionVectors' resolution
    bool depthInverted = false;               // reversed Z
    ID3D12Resource* motionVectors = nullptr;  // shader-readable state
    uint32_t mvWidth = 0, mvHeight = 0;       // region of depth / motionVectors holding this frame
    float mvScaleX = 0, mvScaleY = 0;         // vector value -> color pixels (current -> previous)
    ID3D12Resource* output = nullptr;         // UNORDERED_ACCESS, color's size and format
    bool reset = false;
};

// Loads the runtime on the first call (logged), then records one evaluation into in.list.
// Returns false when unavailable or on error (logged; the runtime is then not used again).
// Command list state is NOT restored here. Not thread safe: callers serialize.
bool Evaluate(const Inputs& in, const Options& options);

// "" while untried or working, otherwise why DLSS 5 is unavailable (for the overlay).
const char* UnavailableReason();

// Releases the feature and NGX, while the device still exists (process exit).
void Shutdown(ID3D12Device* device);

}  // namespace dsru::upscaler::dlss_nr
