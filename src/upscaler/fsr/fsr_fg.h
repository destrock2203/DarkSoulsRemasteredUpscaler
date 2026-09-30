#pragma once

#include "framegen/framegen.h"

#include <d3d12.h>
#include <dxgi1_4.h>

// FSR frame generation (FidelityFX API): the game's swapchain is FidelityFX's frame interpolation
// swapchain, which generates and paces the extra frame on its own queue. Each frame: the prepare
// pass (depth, motion vectors, camera) on the game's command list, and the configuration (on / off,
// HUD-less scene to extract the HUD with). One generated frame (2x).
// Runs on any GPU. Built as no-ops without the SDK (DSRU_HAS_FIDELITYFX). See framegen/framegen.h.
namespace dsru::fsr_fg {

bool CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, DXGI_SWAP_CHAIN_DESC* desc,
                     IDXGISwapChain** out, HRESULT& result);
bool CreateSwapChainForHwnd(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
                            const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                            IDXGISwapChain1** out, HRESULT& result);

void Set(bool enabled, uint32_t framesToGenerate);
bool Wanted();
bool Tag(const UpscaleInputs& in, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState);
void BeforePresent(bool gameFrame);
void AfterPresent();
void OnBeforeResize();
void OnColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);
// Present thread, GPU idle: frame generation torn down before the swapchain's last reference is
// released (hot switch to another backend).
void Detach();
framegen::Status GetStatus();

}  // namespace dsru::fsr_fg
