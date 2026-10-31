#pragma once

#include <d3d12.h>

#include <cstdint>

// Sharpening filters on the D3D12 bridge (shaders/sharpen.hlsl), one compute dispatch: AMD FidelityFX
// RCAS and a log-space Laplacian. NVIDIA Image Scaling's sharpening goes through Streamline instead
// (streamline::EvaluateNis).
namespace dsru::render::sharpen {

// Records into `list`: dst = RCAS(src). Both width x height, RGBA16F, display-referred; `src` in a
// shader-readable state, `dst` in UNORDERED_ACCESS (left there). `sharpness` 0 - 1 as FSR's (1 =
// strongest). Sets the list's descriptor heap, compute root signature and pipeline state. Returns false
// (nothing recorded) if the pass isn't available.
bool Rcas(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
          uint32_t width, uint32_t height, float sharpness);

// The same with the Laplacian filter; `sharpness` 0 - 1.
bool Laplacian(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
               uint32_t width, uint32_t height, float sharpness);

}  // namespace dsru::render::sharpen
