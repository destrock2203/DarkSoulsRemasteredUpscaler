#pragma once

#include "framegen/framegen.h"

#include <d3d12.h>
#include <dxgi1_4.h>

// XeSS frame generation (libxess_fg.dll): the game's swapchain is XeSS-FG's proxy swapchain, which
// generates and paces the extra frames. It requires Intel's XeLL latency reduction (libxell.dll),
// driven like Reflex around each Present. Inputs are tagged like Streamline's: depth / motion
// vectors valid until the next Present, the HUD-less scene copied right away, the camera matrices.
// Runs on Intel Arc and on other GPUs with Shader Model 6.4. Built as no-ops without the SDK
// (DSRU_HAS_XESS). See framegen/framegen.h.
namespace dsru::xefg {

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
framegen::Status GetStatus();
// Hot switch to another backend, on the present thread with the GPU idle: frame generation off
// before the proxy swapchain's last reference is released, then the XeSS-FG and XeLL contexts
// destroyed after (XeSS-FG refuses while the proxy is referenced).
void DetachBeforeRelease();
void DetachAfterRelease();

}  // namespace dsru::xefg
