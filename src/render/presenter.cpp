#include "render/presenter.h"

#include "core/config.h"
#include "core/log.h"
#include "core/perf.h"
#include "framegen/framegen.h"
#include "game/high_fps.h"
#include "hooks/d3d11_hooks.h"
#include "overlay/overlay.h"
#include "render/bridge.h"
#include "render/d3d11_util.h"
#include "render/upscale.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace dsru::render::presenter {

namespace {

std::atomic<HWND> g_window{nullptr};
class GameSwapChain;
std::atomic<GameSwapChain*> g_instance{nullptr};

class GameSwapChain final : public IDXGISwapChain1 {
public:
    static HRESULT Create(IDXGIFactory* factory, ID3D11Device* device, const DXGI_SWAP_CHAIN_DESC& desc,
                          IDXGISwapChain** out) {
        auto* swapChain = new GameSwapChain(factory, device, desc);
        if (!swapChain->Initialize()) {
            swapChain->Release();
            return E_FAIL;
        }
        *out = swapChain;
        return S_OK;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1)) {
            *object = static_cast<IDXGISwapChain1*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (refs == 0) delete this;
        return refs;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override {
        return real_ ? real_->SetPrivateData(name, size, data) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override {
        return real_ ? real_->SetPrivateDataInterface(name, unknown) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override {
        return real_ ? real_->GetPrivateData(name, size, data) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override { return factory_->QueryInterface(riid, parent); }

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override { return device_->QueryInterface(riid, device); }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override { return DoPresent(syncInterval, flags); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) override {
        if (!surface) return E_POINTER;
        if (buffer != 0 || !backBuffer_) return DXGI_ERROR_INVALID_CALL;  // discard model: buffer 0 only
        return backBuffer_.d3d11->QueryInterface(riid, surface);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) override {
        if (forced_) {
            // The forced borderless window becomes the game's own fullscreen, or is left.
            LeaveBorderless();
            forced_ = false;
        }
        if (fullscreen && !fullscreen_) EnterBorderless();
        if (!fullscreen && fullscreen_) LeaveBorderless();
        log::Info("Presenter: the game {} fullscreen (borderless window)", fullscreen ? "entered" : "left");
        (void)target;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override {
        if (fullscreen) *fullscreen = fullscreen_;
        if (target) {
            *target = nullptr;
            if (fullscreen_ && real_) real_->GetContainingOutput(target);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        if (!desc) return E_POINTER;
        *desc = desc_;
        desc->BufferDesc.Width = width_;
        desc->BufferDesc.Height = height_;
        desc->Windowed = !fullscreen_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override {
        return Resize(count, width, height, format, flags);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* mode) override {
        if (!mode) return DXGI_ERROR_INVALID_CALL;
        if (!fullscreen_ && mode->Width && mode->Height) {
            RECT rc{0, 0, static_cast<LONG>(mode->Width), static_cast<LONG>(mode->Height)};
            AdjustWindowRectEx(&rc, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
            SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (fullscreen_ && mode->Width && mode->Height && (mode->Width != width_ || mode->Height != height_)) {
            // A resolution change in fullscreen: a display mode change would resize the window, and the game
            // resizes its buffers when its window really changes size (a posted WM_SIZE alone is ignored).
            // The borderless window keeps the monitor's size: it leaves and covers the monitor again.
            log::Info("Presenter: fullscreen resolution {}x{} requested", mode->Width, mode->Height);
            LeaveBorderless();
            EnterBorderless();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override {
        return real_ ? real_->GetContainingOutput(output) : DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override {
        return real_ ? real_->GetFrameStatistics(stats) : DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override {
        return real_ ? real_->GetLastPresentCount(count) : DXGI_ERROR_INVALID_CALL;
    }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
        if (!desc) return E_POINTER;
        *desc = {};
        desc->Width = width_;
        desc->Height = height_;
        desc->Format = desc_.BufferDesc.Format;
        desc->SampleDesc = {1, 0};
        desc->BufferUsage = desc_.BufferUsage;
        desc->BufferCount = desc_.BufferCount;
        desc->Scaling = DXGI_SCALING_STRETCH;
        desc->SwapEffect = desc_.SwapEffect;
        desc->Flags = desc_.Flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc) override {
        if (!desc) return E_POINTER;
        *desc = {};
        desc->RefreshRate = desc_.BufferDesc.RefreshRate;
        desc->Windowed = !fullscreen_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* hwnd) override {
        if (!hwnd) return E_POINTER;
        *hwnd = hwnd_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS*) override {
        return DoPresent(syncInterval, flags);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override {
        if (output) *output = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* color) override {
        if (color) *color = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* rotation) override {
        if (rotation) *rotation = DXGI_MODE_ROTATION_IDENTITY;
        return S_OK;
    }

    Rect Fit() const {
        Rect r{0, 0, outW_, outH_};
        if (!width_ || !height_ || !outW_ || !outH_) return r;
        const double game = static_cast<double>(width_) / height_, out = static_cast<double>(outW_) / outH_;
        if (game > out + 1e-3)
            r.height = static_cast<UINT>(outW_ / game + 0.5);
        else if (game < out - 1e-3)
            r.width = static_cast<UINT>(outH_ * game + 0.5);
        r.x = (outW_ - r.width) / 2;
        r.y = (outH_ - r.height) / 2;
        return r;
    }
    void GetSizes(UINT& gw, UINT& gh, UINT& ow, UINT& oh) const {
        gw = width_;
        gh = height_;
        ow = outW_;
        oh = outH_;
    }
    // clearBars: black around the game's image (other aspect ratio) - not once the frame's image is drawn.
    ID3D11RenderTargetView* Final(bool clearBars = true) {
        if (!bridge::EnsureShared(final_, outW_, outH_, SwapChainFormat(), bridge::kShaderResource | bridge::kRenderTarget,
                                  L"DSRUpscaler final image"))
            return nullptr;
        const Rect r = Fit();
        if (clearBars && (r.width != outW_ || r.height != outH_)) {
            const float black[4] = {0, 0, 0, 1};
            bridge::Context11()->ClearRenderTargetView(final_.rtv.Get(), black);
        }
        return final_.rtv.Get();
    }
    void MarkFinalDrawn() { finalMarked_ = true; }

private:
    GameSwapChain(IDXGIFactory* factory, ID3D11Device* device, const DXGI_SWAP_CHAIN_DESC& desc)
        : factory_(factory), device_(device), desc_(desc), hwnd_(desc.OutputWindow) {}

    ~GameSwapChain() {
        if (g_instance == this) g_instance = nullptr;
        bridge::WaitIdle();
        if (fullscreen_) LeaveBorderless();
        real_.Reset();
        backBuffer_.Reset();
        if (g_window == hwnd_) g_window = nullptr;
        log::Info("Presenter: game swapchain released");
    }

    void ClientSize(UINT& width, UINT& height) const {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        width = std::max<LONG>(1, rc.right - rc.left);
        height = std::max<LONG>(1, rc.bottom - rc.top);
    }

    DXGI_FORMAT SwapChainFormat() const {
        switch (desc_.BufferDesc.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            default: return desc_.BufferDesc.Format;
        }
    }

    bool Initialize() {
        width_ = desc_.BufferDesc.Width;
        height_ = desc_.BufferDesc.Height;
        if (!width_ || !height_) ClientSize(width_, height_);
        if (!CreateBackBuffer()) return false;

        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(bridge::Factory()->QueryInterface(IID_PPV_ARGS(&factory5))))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        tearing_ = tearing != FALSE;

        ClientSize(outW_, outH_);
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = outW_;
        d.Height = outH_;
        d.Format = SwapChainFormat();
        d.SampleDesc = {1, 0};
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 3;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        swapChainFlags_ = d.Flags;
        ComPtr<IDXGIFactory2> factory2;
        bridge::Factory()->QueryInterface(IID_PPV_ARGS(&factory2));
        {
            bridge::OwnObjectsScope scope;
            IDXGISwapChain1* created = framegen::CreateSwapChain(factory2.Get(), bridge::Queue(), hwnd_, d);
            if (!created) {
                log::Error("Presenter: the D3D12 swapchain could not be created");
                return false;
            }
            real_.Attach(created);
        }
        if (FAILED(real_.As(&real3_))) {
            log::Error("Presenter: the D3D12 swapchain lacks IDXGISwapChain3");
            return false;
        }
        // Alt+Enter is the game's business (it calls SetFullscreenState on us).
        factory2->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        g_window = hwnd_;
        g_instance = this;
        FillScreenIfSmaller();
        log::Info("Presenter: game swapchain {}x{} (output {}x{}) format {} ({} buffers, swap effect {}) presented through D3D12{}",
                  width_, height_, outW_, outH_, static_cast<int>(desc_.BufferDesc.Format), desc_.BufferCount,
                  static_cast<int>(desc_.SwapEffect), tearing_ ? ", tearing allowed" : "");
        return true;
    }

    bool CreateBackBuffer() {
        return bridge::EnsureShared(backBuffer_, width_, height_, desc_.BufferDesc.Format,
                                    bridge::kShaderResource | bridge::kRenderTarget, L"DSRUpscaler game back buffer");
    }

    HRESULT Resize(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
        if (!width || !height) ClientSize(width, height);
        log::Info("Presenter: ResizeBuffers({}, {}x{}, format {}, flags 0x{:x})", count, width, height,
                  static_cast<int>(format), flags);
        bridge::WaitIdle();
        framegen::OnBeforeResize();
        overlay::OnBeforeResize();
        if (format != DXGI_FORMAT_UNKNOWN) desc_.BufferDesc.Format = format;
        if (count) desc_.BufferCount = count;
        width_ = width;
        height_ = height;
        backBuffer_.Reset();
        ResizeOutput(true);
        if (!CreateBackBuffer()) return E_OUTOFMEMORY;
        FillScreenIfSmaller();
        return S_OK;
    }

    // The game renders below the monitor's resolution in a window: the window covers the monitor, so the
    // upscaler has the screen to fill ([Upscaler] FillScreen). The game is still told it is windowed.
    void FillScreenIfSmaller() {
        if (fullscreen_ || !config::Get().fillScreen) return;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        const UINT mw = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
        const UINT mh = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
        const bool smaller = width_ < mw || height_ < mh;
        if (forced_ && !smaller) {  // back to the monitor's resolution: the game's own window again
            LeaveBorderless();
            forced_ = false;
        }
        if (forced_ || !smaller) return;
        log::Info("Presenter: the game renders {}x{} on a {}x{} monitor - the window covers the monitor", width_,
                  height_, mw, mh);
        EnterBorderless();
        forced_ = true;
        fullscreen_ = FALSE;
    }

    // The D3D12 swapchain follows the window's client area (the monitor in fullscreen).
    void ResizeOutput(bool idle) {
        UINT w = 0, h = 0;
        ClientSize(w, h);
        if (w == outW_ && h == outH_) return;
        if (!idle) {
            bridge::WaitIdle();
            framegen::OnBeforeResize();
        }
        final_.Reset();
        const HRESULT hr = real_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
        if (FAILED(hr)) {
            log::Error("Presenter: D3D12 swapchain ResizeBuffers({}x{}) failed: 0x{:08x}", w, h, static_cast<unsigned>(hr));
            return;
        }
        outW_ = w;
        outH_ = h;
        log::Info("Presenter: output {}x{} (the game renders {}x{})", w, h, width_, height_);
    }

    void EnterBorderless() {
        savedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        savedExStyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        savedPlacement_.length = sizeof(savedPlacement_);
        GetWindowPlacement(hwnd_, &savedPlacement_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedStyle_ & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER)) | WS_POPUP);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedExStyle_ & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = TRUE;
        ResizeOutput(false);
    }

    void LeaveBorderless() {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, savedExStyle_);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        fullscreen_ = FALSE;
        if (real_) ResizeOutput(false);
    }

    HRESULT DoPresent(UINT syncInterval, UINT flags) {
        if (flags & DXGI_PRESENT_TEST) return S_OK;
        perf::PresentEntered();
        game::high_fps::OnFrame();
        upscale::OnPresent();
        // What is shown: the game's back buffer as it is, or an output-size image (the upscaled frame
        // drawn by render/upscale.h, or the back buffer stretched to the output).
        bridge::SharedTexture* source = &backBuffer_;
        if (finalMarked_ || outW_ != width_ || outH_ != height_) {
            // Marked: the upscaled frame is already in final_ (render/upscale.h drew it with its bars).
            if (Final(!finalMarked_)) {
                if (!finalMarked_) {
                    const Rect r = Fit();
                    d3d11::BlitRect(bridge::Context11(), backBuffer_.srv.Get(), final_.rtv.Get(), r.x, r.y, r.width,
                                    r.height);
                }
                source = &final_;
            }
        }
        finalMarked_ = false;
        overlay::Render(bridge::Context11(), source->rtv.Get(), source->width, source->height, hwnd_);

        UpscaleInputs inputs;
        ID3D12Resource* hudless = nullptr;
        const bool hasInputs = upscale::TakeFrameInputs(inputs, hudless);
        const bool gameFrame = hasInputs && framegen::Wanted();

        const uint64_t ready = bridge::SignalFrom11();
        bridge::WaitOn12(ready);
        ID3D12GraphicsCommandList* list = bridge::BeginList();
        if (gameFrame) {
            inputs.list = list;
            inputs.outputSize = {source->width, source->height};
            framegen::Tag(inputs, hudless, D3D12_RESOURCE_STATE_COMMON);
        }
        ComPtr<ID3D12Resource> target;
        if (SUCCEEDED(real_->GetBuffer(real3_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target)))) {
            D3D12_RESOURCE_BARRIER before[2] = {
                bridge::Transition(target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
                bridge::Transition(source->d3d12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
            list->ResourceBarrier(2, before);
            list->CopyResource(target.Get(), source->d3d12.Get());
            D3D12_RESOURCE_BARRIER after[2] = {
                bridge::Transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                bridge::Transition(source->d3d12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)};
            list->ResourceBarrier(2, after);
        }
        bridge::Submit(list);
        target.Reset();

        perf::NativePresentCalled();
        framegen::BeforePresent(gameFrame);
        if (config::Get().vsync >= 0) syncInterval = static_cast<UINT>(config::Get().vsync);
        UINT presentFlags = 0;
        if (syncInterval == 0 && tearing_ && !framegen::GetStatus().active) presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
        const HRESULT hr = real_->Present(syncInterval, presentFlags);
        framegen::AfterPresent();
        bridge::WaitOn11(bridge::SignalFrom12());
        perf::PresentReturned();
        if (FAILED(hr)) {
            // Once the D3D12 device is lost every Present fails the same way: the first one logs why.
            static bool lost = false;
            const bool removed = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                                 hr == DXGI_ERROR_DEVICE_HUNG;
            if (!removed)
                log::Error("Presenter: Present failed: 0x{:08x}", static_cast<unsigned>(hr));
            else if (!lost) {
                lost = true;
                log::Error("Presenter: Present failed: 0x{:08x}, the D3D12 device is lost (reason 0x{:08x}) - the "
                           "image stops here, restart the game",
                           static_cast<unsigned>(hr), static_cast<unsigned>(bridge::Device12()->GetDeviceRemovedReason()));
            }
        }
        return hr;
    }

    std::atomic<ULONG> refs_{1};
    ComPtr<IDXGIFactory> factory_;
    ComPtr<ID3D11Device> device_;
    DXGI_SWAP_CHAIN_DESC desc_;
    HWND hwnd_;
    UINT width_ = 0, height_ = 0;  // the game's back buffer
    UINT outW_ = 0, outH_ = 0;      // the D3D12 swapchain
    bridge::SharedTexture backBuffer_;
    bridge::SharedTexture final_;   // output size, when it differs from the game's or upscale drew it
    bool finalMarked_ = false;
    ComPtr<IDXGISwapChain1> real_;
    ComPtr<IDXGISwapChain3> real3_;
    UINT swapChainFlags_ = 0;
    bool tearing_ = false;
    BOOL fullscreen_ = FALSE;
    bool forced_ = false;  // borderless without the game asking (FillScreenIfSmaller)
    LONG_PTR savedStyle_ = 0, savedExStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_{};
};

}  // namespace

bool CreateSwapChain(IDXGIFactory* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC& desc, IDXGISwapChain** out,
                     HRESULT& result) {
    if (!config::Get().presenter || !bridge::Ready() || !out) return false;
    ComPtr<ID3D11Device> d3d11;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&d3d11))) || d3d11.Get() != hooks::d3d11::Device()) return false;
    result = GameSwapChain::Create(factory, d3d11.Get(), desc, out);
    if (FAILED(result)) {
        log::Error("Presenter: failed (0x{:08x}) - the game gets its own swapchain, no frame generation",
                   static_cast<unsigned>(result));
        return false;
    }
    return true;
}

HWND Window() { return g_window.load(); }

bool Sizes(UINT& gameWidth, UINT& gameHeight, UINT& outputWidth, UINT& outputHeight) {
    GameSwapChain* instance = g_instance.load();
    if (!instance) return false;
    instance->GetSizes(gameWidth, gameHeight, outputWidth, outputHeight);
    return outputWidth && outputHeight;
}

ID3D11RenderTargetView* FinalTarget() {
    GameSwapChain* instance = g_instance.load();
    return instance ? instance->Final() : nullptr;
}

Rect OutputRect() {
    GameSwapChain* instance = g_instance.load();
    return instance ? instance->Fit() : Rect{};
}

void MarkFinal() {
    if (GameSwapChain* instance = g_instance.load()) instance->MarkFinalDrawn();
}

}  // namespace dsru::render::presenter
