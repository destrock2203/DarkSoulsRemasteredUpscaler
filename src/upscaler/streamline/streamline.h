#pragma once

#include "upscaler/upscaler_types.h"

#include <d3d12.h>
#include <dxgi1_4.h>

// NVIDIA Streamline (DLSS, DLSS-G frame generation, Reflex / PCL markers), integrated in "manual
// hooking" mode since we don't own device / swapchain creation:
//  1. Init() on the game's first D3D12CreateDevice, before any swapchain exists: verify the
//     signature of sl.interposer.dll, load it, slInit with eUseManualHooking.
//  2. OnDeviceCreated(): slSetD3DDevice + feature support check on the device's adapter.
//  3. When DLSS-G is available, the game's command queues and swapchain are created through
//     Streamline's proxies (CreateCommandQueue, ProxyFactory), as DLSS-G requires; frame generation
//     is then turned on and off at any time.
//     Otherwise the swapchain is upgraded after creation (UpgradeSwapChain). Either way the game
//     gets Streamline's proxy swapchain, so Streamline sees every Present.
//  4. Every frame, one frame token (CurrentToken) shared by DLSS, the frame generation tags and the
//     PCL markers around Present (BeforePresent / AfterPresent).
// Built as no-ops without the SDK (DSRU_HAS_STREAMLINE).
namespace dsru::streamline {

// Loads and initializes Streamline once. Safe to call repeatedly; only the first call works.
void Init();

// True while the calling thread is inside a Streamline call: D3D objects created then belong to
// Streamline (e.g. a probe device), not to the game.
bool InsideStreamline();

void OnDeviceCreated(ID3D12Device* device);

// Replaces *swapChain with Streamline's proxy. Returns false (and leaves it untouched) when
// Streamline isn't active or DLSS isn't supported (nothing for the proxy to do).
bool UpgradeSwapChain(IDXGISwapChain1** swapChain);

// Streamline's proxy for a DXGI factory (a reference the caller releases), or null when frame
// generation isn't enabled at startup or available (the swapchain is then upgraded after creation).
IDXGIFactory* ProxyFactory(IDXGIFactory* native);
// Creates a command queue through Streamline's proxy device (DLSS-G hooks queue creation) and sets
// `handled`; when frame generation isn't enabled at startup, `handled` is false and nothing is done.
HRESULT CreateCommandQueue(ID3D12Device* native, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** queue,
                           bool& handled);
// The game's swapchain was created through ProxyFactory: frame generation can be turned on.
void OnSwapChainAttached();
// Present thread: frame generation off, before the proxy swapchain is released (hot switch to
// another backend).
void DetachFrameGen();
// Present thread, after the proxy swapchain is released: unloads sl.dlss_g so it lets go of the
// native swapchain (and the window). ProxyFactory reloads it when DLSS-G is switched back to.
void UnloadFrameGen();
// The native object behind one of Streamline's proxies (a reference the caller releases), or null.
void* NativeInterface(void* proxy);

// Streamline initialized, device set and DLSS supported on the game's adapter.
bool DlssAvailable();

// DLSS's preferred render resolution for an output size and quality mode.
bool DlssOptimalRenderSize(Resolution output, QualityMode mode, Resolution& render);

// One DLSS evaluation (see UpscaleInputs). `preset`: sl::DLSSPreset value (0 = NVIDIA's default),
// applied to every mode. Returns false on any Streamline error (logged). Command list state is NOT
// restored here.
bool EvaluateDlss(const UpscaleInputs& inputs, uint32_t preset);

// --- NVIDIA Image Scaling (NIS), sharpening only ---

// Streamline initialized and NIS supported on the game's adapter (any GPU vendor), and no NIS
// evaluation has failed.
bool NisAvailable();

// Records NIS sharpening into `list`: output = sharpen(input), both width x height, `output` with
// UAV support. States as the resources are when this executes; each is returned to its own.
// `sharpness` 0 - 1. Uses its own viewport (DLSS keeps viewport 0). Returns false on any Streamline
// error (logged once; NIS then stays off). Command list state is NOT restored here.
bool EvaluateNis(ID3D12GraphicsCommandList* list, ID3D12Resource* input, D3D12_RESOURCE_STATES inputState,
                 ID3D12Resource* output, D3D12_RESOURCE_STATES outputState, uint32_t width, uint32_t height,
                 float sharpness);

// --- Frame generation (DLSS-G) ---

// DLSS-G supported on this adapter (and loaded).
bool FrameGenAvailable();
// The game's swapchain was created for frame generation (enabled at startup).
bool FrameGenAttached();
// User setting; applied at the next Present. `framesToGenerate`: 1 = 2x, 2 = 3x, 3 = 4x.
void SetFrameGen(bool enabled, uint32_t framesToGenerate);
// Enabled by the user and available: the game should provide the inputs (TagFrameGen).
bool FrameGenWanted();

struct FrameGenStatus {
    bool available = false;
    bool attached = false;       // the swapchain was created for frame generation (at startup)
    bool enabled = false;   // user setting
    bool active = false;   // turned on in Streamline right now
    uint32_t framesToGenerate = 1;
    uint32_t maxFramesToGenerate = 0;
    uint32_t framesPresented = 0;  // per game frame, last reported by DLSS-G
    uint32_t status = 0;           // sl::DLSSGStatus bits
};
FrameGenStatus GetFrameGenStatus();

// Tags this frame's frame generation inputs, from the last scene draw before the HUD: depth and
// motion vectors (in.depth / in.motionVectors, render size extents) and the HUD-less color at the
// output size (copied by Streamline now). Also sets the frame's camera constants from `in`.
bool TagFrameGen(const UpscaleInputs& in, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState);

// From the Present hook of the game's swapchain, around the native Present. `gameFrame`: the frame
// being presented was a game frame with frame generation inputs (not a menu / loading screen).
void BeforePresent(bool gameFrame);
void AfterPresent();
// Frame generation must be off while the swapchain is resized.
void OnBeforeResize();

// slShutdown, while the device still exists. Must happen before process exit tears DLLs down:
// otherwise nvngx_dlss.dll's own cleanup calls into an already unloaded sl.common.dll.
void Shutdown();

}  // namespace dsru::streamline
