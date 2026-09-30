#pragma once

struct IUnknown;

// DXGI: the game's swapchain. IDXGIFactory::CreateSwapChain (the game's only way to create one) is
// hooked on every factory: with the presenter the game gets render/presenter.h's swapchain; without,
// its own, whose Present is hooked for the overlay, the FPS limit and Streamline's frame tokens.
namespace dsru::hooks::dxgi {

void OnFactoryCreated(IUnknown* factory);

}  // namespace dsru::hooks::dxgi
