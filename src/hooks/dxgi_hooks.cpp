#include "hooks/dxgi_hooks.h"

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "framegen/framegen.h"
#include "game/high_fps.h"
#include "hooks/d3d11_hooks.h"
#include "hooks/d3d11_vtables.h"
#include "hooks/hook_manager.h"
#include "overlay/overlay.h"
#include "render/bridge.h"
#include "render/presenter.h"
#include "render/upscale.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace dsru::hooks::dxgi {

namespace {

using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

CreateSwapChain_t oCreateSwapChain = nullptr;
Present_t oPresent = nullptr;
ResizeBuffers_t oResizeBuffers = nullptr;

// The game's native swapchain (presenter off or failed) and its back buffer's view for the overlay.
IDXGISwapChain* g_native = nullptr;
ComPtr<ID3D11RenderTargetView> g_nativeRtv;
UINT g_nativeWidth = 0, g_nativeHeight = 0;

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
    if (self != g_native || (flags & DXGI_PRESENT_TEST)) return oPresent(self, syncInterval, flags);
    perf::PresentEntered();
    game::high_fps::OnFrame();
    render::upscale::OnPresent();
    if (!g_nativeRtv) {
        ComPtr<ID3D11Texture2D> buffer;
        if (SUCCEEDED(self->GetBuffer(0, IID_PPV_ARGS(&buffer)))) {
            D3D11_TEXTURE2D_DESC desc;
            buffer->GetDesc(&desc);
            g_nativeWidth = desc.Width;
            g_nativeHeight = desc.Height;
            if (ID3D11Device* device = d3d11::Device()) device->CreateRenderTargetView(buffer.Get(), nullptr, &g_nativeRtv);
        }
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    self->GetDesc(&desc);
    overlay::Render(d3d11::Immediate(), g_nativeRtv.Get(), g_nativeWidth, g_nativeHeight, desc.OutputWindow);
    perf::NativePresentCalled();
    const HRESULT hr = oPresent(self, syncInterval, flags);
    framegen::AfterPresent();  // Streamline's frame tokens
    perf::PresentReturned();
    return hr;
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                          UINT flags) {
    if (self == g_native) {
        g_nativeRtv.Reset();
        overlay::OnBeforeResize();
    }
    return oResizeBuffers(self, count, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                            IDXGISwapChain** out) {
    // The bridge's own swapchain (and whatever frame generation creates for it).
    if (render::bridge::CreatingOwnObjects() || !device || !desc || !out) return oCreateSwapChain(self, device, desc, out);
    log::Info("Game CreateSwapChain: {}x{} format {} buffers {} swap effect {} flags 0x{:x} windowed {}",
              desc->BufferDesc.Width, desc->BufferDesc.Height, static_cast<int>(desc->BufferDesc.Format),
              desc->BufferCount, static_cast<int>(desc->SwapEffect), desc->Flags, desc->Windowed);
    if (HRESULT hr = E_FAIL; render::presenter::CreateSwapChain(self, device, *desc, out, hr)) return hr;

    const HRESULT hr = oCreateSwapChain(self, device, desc, out);
    if (SUCCEEDED(hr) && *out && !g_native) {
        g_native = *out;
        InstallVTable(*out, vt::kSwapChain_Present, &hkPresent, &oPresent, "IDXGISwapChain::Present");
        InstallVTable(*out, vt::kSwapChain_ResizeBuffers, &hkResizeBuffers, &oResizeBuffers, "IDXGISwapChain::ResizeBuffers");
        log::Info("The game presents with its own swapchain (no frame generation)");
    }
    return hr;
}

}  // namespace

void OnFactoryCreated(IUnknown* factory) {
    if (!config::Get().enabled) return;
    ComPtr<IDXGIFactory> f;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f))))
        Install(VTableEntry(f.Get(), vt::kFactory_CreateSwapChain), &hkCreateSwapChain, &oCreateSwapChain,
                "IDXGIFactory::CreateSwapChain");
}

}  // namespace dsru::hooks::dxgi
