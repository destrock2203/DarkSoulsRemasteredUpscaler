#pragma once

#include "upscaler/upscaler_types.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

// Frame generation, whichever backend runs it: DLSS-G (upscaler/streamline), FSR frame generation
// (upscaler/fsr/fsr_fg.h) or XeSS-FG (upscaler/xess/xess_fg.h). The backend is chosen at startup
// ([FrameGeneration] Backend): it creates the D3D12 swapchain the presenter (render/presenter.h)
// shows the game's frames on. Turning frame generation on and off works at any time.
namespace dsru::framegen {

// The presenter's swapchain for `hwnd`, created by the configured backend (or a plain one when it is
// unavailable). `queue`: the bridge's queue (created through Streamline's proxy for DLSS-G).
IDXGISwapChain1* CreateSwapChain(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
                                 const DXGI_SWAP_CHAIN_DESC1& desc);

// The backend behind the swapchain (the configured one, or the one that attached).
Backend Selected();

// User setting, applied at the next Present. `framesToGenerate`: 1 = 2x, 2 = 3x, 3 = 4x.
void Set(bool enabled, uint32_t framesToGenerate);
// From the menu: also saved to the ini.
void SetByUser(bool enabled, uint32_t framesToGenerate);
// Enabled and attached: the frame's inputs should be captured.
bool Wanted();

// This frame's inputs (depth, motion vectors, camera, HUD-less image), recorded into in.list.
bool Tag(const UpscaleInputs& in, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState);

// Around the presenter's Present. `gameFrame`: this frame was tagged.
void BeforePresent(bool gameFrame);
void AfterPresent();
void OnBeforeResize();

struct Status {
    Backend backend = Backend::Dlss;
    bool available = false;  // the backend's runtime loaded and supported
    bool attached = false;   // the swapchain was created for it
    bool enabled = false;    // user setting
    bool active = false;     // generating right now
    uint32_t framesToGenerate = 1;
    uint32_t maxFramesToGenerate = 0;
    uint32_t framesPresented = 0;  // per game frame, last reported
    uint32_t status = 0;           // backend-specific problem code, 0 = fine
};
Status GetStatus();

}  // namespace dsru::framegen
