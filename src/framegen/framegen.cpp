#include "framegen/framegen.h"

#include "core/config.h"
#include "core/log.h"
#include "upscaler/fsr/fsr_fg.h"
#include "upscaler/streamline/streamline.h"
#include "upscaler/xess/xess_fg.h"

#include <wrl/client.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace dsru::framegen {

namespace {

std::atomic<Backend> g_backend{Backend::Dlss};
std::atomic<bool> g_attached{false};
std::atomic<bool> g_enabled{false};
std::atomic<uint32_t> g_frames{1};

void Apply() {
    const bool on = g_enabled.load();
    const uint32_t frames = g_frames.load();
    switch (g_backend.load()) {
        case Backend::Dlss: streamline::SetFrameGen(on, frames); break;
        case Backend::Fsr: fsr_fg::Set(on, frames); break;
        case Backend::Xess: xefg::Set(on, frames); break;
    }
}

}  // namespace

IDXGISwapChain1* CreateSwapChain(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
                                 const DXGI_SWAP_CHAIN_DESC1& desc) {
    const Backend wanted = config::Get().frameGenerationBackend;
    IDXGISwapChain1* swapChain = nullptr;
    HRESULT hr = E_FAIL;
    g_backend = wanted;
    switch (wanted) {
        case Backend::Fsr:
            g_attached = fsr_fg::CreateSwapChainForHwnd(factory, queue, hwnd, &desc, nullptr, &swapChain, hr) && swapChain;
            break;
        case Backend::Xess:
            g_attached = xefg::CreateSwapChainForHwnd(factory, queue, hwnd, &desc, nullptr, &swapChain, hr) && swapChain;
            break;
        case Backend::Dlss:
            if (IDXGIFactory* proxy = streamline::ProxyFactory(factory)) {
                ComPtr<IDXGIFactory2> proxy2;
                if (SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&proxy2))) &&
                    SUCCEEDED(proxy2->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swapChain)) &&
                    swapChain) {
                    streamline::OnSwapChainAttached();
                    g_attached = true;
                }
                // The proxy factory is kept for the process lifetime (Streamline's requirement).
            }
            break;
    }
    if (!g_attached) {
        if (swapChain) swapChain->Release(), swapChain = nullptr;
        log::Warn("Frame generation ({}) unavailable - presenting without it", ToString(wanted));
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swapChain))) return nullptr;
    } else {
        log::Info("Frame generation: {} owns the swapchain", ToString(wanted));
    }
    Set(config::Get().frameGeneration, static_cast<uint32_t>(config::Get().generatedFrames));
    return swapChain;
}

Backend Selected() { return g_backend.load(); }

void Set(bool enabled, uint32_t framesToGenerate) {
    g_enabled = enabled;
    g_frames = framesToGenerate;
    Apply();
}

void SetByUser(bool enabled, uint32_t framesToGenerate) {
    Set(enabled, framesToGenerate);
    config::SaveFrameGeneration(enabled, static_cast<int>(framesToGenerate));
    log::Info("Frame generation {} ({}x)", enabled ? "on" : "off", framesToGenerate + 1);
}

bool Wanted() {
    if (!g_attached || !g_enabled) return false;
    switch (g_backend.load()) {
        case Backend::Dlss: return streamline::FrameGenWanted();
        case Backend::Fsr: return fsr_fg::Wanted();
        case Backend::Xess: return xefg::Wanted();
    }
    return false;
}

bool Tag(const UpscaleInputs& in, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState) {
    switch (g_backend.load()) {
        case Backend::Dlss: return streamline::TagFrameGen(in, hudless, hudlessState);
        case Backend::Fsr: return fsr_fg::Tag(in, hudless, hudlessState);
        case Backend::Xess: return xefg::Tag(in, hudless, hudlessState);
    }
    return false;
}

void BeforePresent(bool gameFrame) {
    switch (g_backend.load()) {
        case Backend::Dlss: streamline::BeforePresent(gameFrame); break;
        case Backend::Fsr: fsr_fg::BeforePresent(gameFrame); break;
        case Backend::Xess: xefg::BeforePresent(gameFrame); break;
    }
}

void AfterPresent() {
    // Streamline's frame tokens advance every frame (DLSS needs them even without frame generation).
    streamline::AfterPresent();
    switch (g_backend.load()) {
        case Backend::Fsr: fsr_fg::AfterPresent(); break;
        case Backend::Xess: xefg::AfterPresent(); break;
        default: break;
    }
}

void OnBeforeResize() {
    switch (g_backend.load()) {
        case Backend::Dlss: streamline::OnBeforeResize(); break;
        case Backend::Fsr: fsr_fg::OnBeforeResize(); break;
        default: break;
    }
}

Status GetStatus() {
    Status s;
    switch (g_backend.load()) {
        case Backend::Dlss: {
            const auto sl = streamline::GetFrameGenStatus();
            s.available = sl.available;
            s.attached = sl.attached;
            s.enabled = sl.enabled;
            s.active = sl.active;
            s.framesToGenerate = sl.framesToGenerate;
            s.maxFramesToGenerate = sl.maxFramesToGenerate;
            s.framesPresented = sl.framesPresented;
            s.status = sl.status;
            break;
        }
        case Backend::Fsr: s = fsr_fg::GetStatus(); break;
        case Backend::Xess: s = xefg::GetStatus(); break;
    }
    s.backend = g_backend.load();
    s.attached = s.attached && g_attached;
    return s;
}

}  // namespace dsru::framegen
