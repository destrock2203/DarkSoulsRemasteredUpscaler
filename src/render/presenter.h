#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>

// The game's swapchain when frame generation is possible ([FrameGeneration] Presenter=1). The game
// renders into a back buffer the D3D12 bridge shares (render/bridge.h) and presents it as usual; each
// Present then:
//     D3D11: the overlay is drawn -> signal D3D12
//     D3D12: frame generation inputs tagged (depth, motion vectors, HUD-less image from
//            render/upscale.h) -> back buffer copied into the D3D12 swapchain -> Present
//     D3D11: waits for the copy before the next frame touches the back buffer
// The D3D12 swapchain belongs to the frame generation backend (framegen/framegen.h). The game's
// exclusive fullscreen becomes a borderless window over the monitor (frame generation needs a flip
// model swapchain; the output looks the same).
namespace dsru::render::presenter {

// From the IDXGIFactory::CreateSwapChain hook: the game's swapchain request. Returns false (nothing
// done) when the presenter is off or the bridge isn't ready; the caller then creates it natively.
bool CreateSwapChain(IDXGIFactory* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC& desc, IDXGISwapChain** out,
                     HRESULT& result);

// The window the game presents to (null before its swapchain exists).
HWND Window();

// The game's back buffer size and the output (the D3D12 swapchain: the window's client area, the
// monitor in fullscreen). They differ when the game renders below the screen's resolution: frames are
// then upscaled (render/upscale.h) or stretched to the output. False before the swapchain exists.
bool Sizes(UINT& gameWidth, UINT& gameHeight, UINT& outputWidth, UINT& outputHeight);
// Where the game's image goes in the output: its aspect ratio fitted and centered (all of it when the
// ratios match). Black bars around it otherwise.
struct Rect {
    UINT x = 0, y = 0, width = 0, height = 0;
};
Rect OutputRect();
// Immediate context: the output-size image the next Present shows, when render/upscale.h draws the
// frame itself (upscaled scene + HUD through the game's final pass). MarkFinal once it is drawn.
ID3D11RenderTargetView* FinalTarget();
void MarkFinal();

}  // namespace dsru::render::presenter
