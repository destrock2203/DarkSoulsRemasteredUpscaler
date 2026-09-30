#include "upscaler/fsr/fsr_fg.h"

#include "core/log.h"

#ifdef DSRU_HAS_FIDELITYFX

#include "upscaler/fsr/ffx_runtime.h"

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>

#include <wrl/client.h>

#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace dsru::fsr_fg {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Tag runs on the game's render threads, the rest on the present thread.
std::mutex g_mutex;

ID3D12Device* g_device = nullptr;
ffxContext g_swapChainContext = nullptr;
IDXGISwapChain4* g_swapChain = nullptr;  // FidelityFX's swapchain, owned by the game
DXGI_FORMAT g_backBufferFormat = DXGI_FORMAT_UNKNOWN;
std::atomic<bool> g_hdr{false};  // HDR10 output (SetColorSpace1)

// The frame generation context and what it was created for.
struct Key {
    uint32_t width = 0, height = 0;
    DXGI_FORMAT hudlessFormat = DXGI_FORMAT_UNKNOWN;
    bool hdr = false;
    bool invertedDepth = false;
    bool operator==(const Key&) const = default;
};
ffxContext g_context = nullptr;
Key g_key;
bool g_createFailed = false;

// HUD-less scene copies: FidelityFX reads the HUD-less image when it presents (its own queue), while
// the game already renders the next frame: one per frame in flight.
ComPtr<ID3D12Resource> g_hudless[2];
uint32_t g_hudlessIndex = 0;

std::atomic<uint64_t> g_frameId{1};  // frame being recorded / presented next; +1 per Present
uint64_t g_taggedFrame = 0;          // frame id of the last Tag
Rect g_generationRect;               // the last Tag's outputRect (empty: the whole back buffer)
std::atomic<bool> g_enabled{false};
bool g_active = false;

ffxReturnCode_t DispatchCallback(ffxDispatchDescFrameGeneration* params, void* context) {
    return ffx::Api().dispatch(static_cast<ffxContext*>(context), &params->header);
}

// The frame's configuration. `hudless` null: no HUD extraction. Called with g_mutex held.
ffxReturnCode_t Configure(bool enabled, ID3D12Resource* hudless, uint64_t frameId) {
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_swapChain;
    c.frameGenerationCallback = DispatchCallback;
    c.frameGenerationCallbackUserContext = &g_context;
    c.frameGenerationEnabled = enabled;
    c.allowAsyncWorkloads = false;
    if (hudless) c.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    // A letterboxed scene: only its region is interpolated (the bars are static).
    c.generationRect = g_generationRect.width && g_generationRect.height
                           ? FfxApiRect2D{static_cast<int32_t>(g_generationRect.x),
                                          static_cast<int32_t>(g_generationRect.y),
                                          static_cast<int32_t>(g_generationRect.width),
                                          static_cast<int32_t>(g_generationRect.height)}
                           : FfxApiRect2D{0, 0, static_cast<int32_t>(g_key.width), static_cast<int32_t>(g_key.height)};
    c.frameID = frameId;
    return ffx::Api().configure(&g_context, &c.header);
}

// Frame generation off, pending presents done, context destroyed. Called with g_mutex held.
void DestroyContext() {
    if (!g_context) return;
    Configure(false, nullptr, g_frameId.load());
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    ffx::Api().dispatch(&g_swapChainContext, &wait.header);
    ffx::Api().destroyContext(&g_context, nullptr);
    g_context = nullptr;
    g_active = false;
    for (auto& t : g_hudless) t.Reset();
}

bool CreateContext(const Key& key) {
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_device;
    ffxCreateContextDescFrameGenerationVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGenerationHudless hudless{};
    hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(key.hudlessFormat);

    ffxCreateContextDescFrameGeneration desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    desc.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    version.header.pNext = &hudless.header;
    desc.flags = key.invertedDepth ? static_cast<uint32_t>(FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) : 0u;
    if (key.hdr) desc.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    desc.displaySize = {key.width, key.height};
    desc.maxRenderSize = {key.width, key.height};
    desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(g_backBufferFormat);

    if (const ffxReturnCode_t r = ffx::Api().createContext(&g_context, &desc.header, nullptr); r != FFX_API_RETURN_OK) {
        log::Error("FSR frame generation: context creation for {}x{} failed: {}", key.width, key.height,
                   ffx::ReturnCode(r));
        g_context = nullptr;
        return false;
    }
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool named = ffx::Api().query(&g_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName;
    log::Info("FSR frame generation {}: context for {}x{} (back buffer fmt={}, HUD-less fmt={}, {})",
              named ? provider.versionName : "?", key.width, key.height, static_cast<int>(g_backBufferFormat),
              static_cast<int>(key.hudlessFormat), key.hdr ? "HDR10" : "SDR");
    return true;
}

// (Re)creates `texture` like `like`, in kShaderRead.
bool EnsureTexture(ComPtr<ID3D12Resource>& texture, const D3D12_RESOURCE_DESC& like) {
    if (texture) {
        const D3D12_RESOURCE_DESC d = texture->GetDesc();
        if (d.Width == like.Width && d.Height == like.Height && d.Format == like.Format) return true;
        texture.Reset();
    }
    D3D12_RESOURCE_DESC desc = like;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.MipLevels = 1;
    desc.DepthOrArraySize = 1;
    desc.Alignment = 0;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kShaderRead, nullptr,
                                                 IID_PPV_ARGS(&texture)))) {
        log::Error("FSR frame generation: failed to create the HUD-less copy");
        return false;
    }
    texture->SetName(L"DSRUpscaler FSR FG HUD-less copy");
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void CopyHudless(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src,
                 D3D12_RESOURCE_STATES srcState) {
    D3D12_RESOURCE_BARRIER before[2], after[2];
    UINT count = 0;
    before[count] = Transition(dst, kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
    after[count++] = Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
    if (srcState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        before[count] = Transition(src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        after[count++] = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
    }
    list->ResourceBarrier(count, before);
    list->CopyResource(dst, src);
    list->ResourceBarrier(count, after);
}

// Camera basis in world space from the world -> view matrix (row-major for column vectors, rows =
// right, up, forward; orthonormal).
void CameraVectors(const float* view, float pos[3], float up[3], float right[3], float forward[3]) {
    for (int i = 0; i < 3; ++i) {
        right[i] = view[0 * 4 + i];
        up[i] = view[1 * 4 + i];
        forward[i] = view[2 * 4 + i];
    }
    const float t[3] = {view[3], view[7], view[11]};
    for (int i = 0; i < 3; ++i) pos[i] = -(t[0] * right[i] + t[1] * up[i] + t[2] * forward[i]);
}

// Creates the frame interpolation swapchain described by `desc`.
bool Attach(ffxCreateContextDescHeader& desc, ID3D12CommandQueue* queue, DXGI_FORMAT format) {
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    desc.pNext = &version.header;
    // On Radeon the swapchain may come from the driver: a crash from here on is inside it.
    log::Info("FSR frame generation: creating the frame interpolation swapchain");
    if (const ffxReturnCode_t r = ffx::Api().createContext(&g_swapChainContext, &desc, nullptr); r != FFX_API_RETURN_OK) {
        log::Error("FSR frame generation: swapchain creation failed: {} - creating the game's swapchain normally",
                   ffx::ReturnCode(r));
        g_swapChainContext = nullptr;
        return false;
    }
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx::Api().query(&g_swapChainContext, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
        log::Info("FSR frame generation: swapchain provider {} (id 0x{:016x})", provider.versionName,
                  provider.versionId);
    queue->GetDevice(IID_PPV_ARGS(&g_device));
    if (g_device) g_device->Release();  // the game's device outlives the swapchain
    g_backBufferFormat = format;
    return true;
}

}  // namespace

bool CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, DXGI_SWAP_CHAIN_DESC* desc,
                     IDXGISwapChain** out, HRESULT& result) {
    if (!desc || !ffx::Load()) return false;
    std::lock_guard lock(g_mutex);
    IDXGISwapChain4* swapChain = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainNewDX12 d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12;
    d.swapchain = &swapChain;
    d.desc = desc;
    d.dxgiFactory = factory;
    d.gameQueue = queue;
    if (!Attach(d.header, queue, desc->BufferDesc.Format) || !swapChain) return false;
    g_swapChain = swapChain;
    *out = swapChain;
    result = S_OK;
    log::Info("FSR frame generation: the game's swapchain is FidelityFX's frame interpolation swapchain ({}x{})",
              desc->BufferDesc.Width, desc->BufferDesc.Height);
    return true;
}

bool CreateSwapChainForHwnd(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
                            const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                            IDXGISwapChain1** out, HRESULT& result) {
    if (!desc || !ffx::Load()) return false;
    std::lock_guard lock(g_mutex);
    DXGI_SWAP_CHAIN_DESC1 desc1 = *desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
    if (fullscreenDesc) fullscreen = *fullscreenDesc;
    IDXGISwapChain4* swapChain = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 d{};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    d.swapchain = &swapChain;
    d.hwnd = hwnd;
    d.desc = &desc1;
    d.fullscreenDesc = fullscreenDesc ? &fullscreen : nullptr;
    d.dxgiFactory = factory;
    d.gameQueue = queue;
    if (!Attach(d.header, queue, desc->Format) || !swapChain) return false;
    g_swapChain = swapChain;
    *out = swapChain;
    result = S_OK;
    log::Info("FSR frame generation: the game's swapchain is FidelityFX's frame interpolation swapchain ({}x{})",
              desc->Width, desc->Height);
    return true;
}

void Set(bool enabled, uint32_t) { g_enabled = enabled; }

bool Wanted() { return g_enabled && g_swapChain; }

bool Tag(const UpscaleInputs& in, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState) {
    if (!Wanted() || !hudless) return false;
    std::lock_guard lock(g_mutex);
    if (!g_swapChain) return false;  // detached while waiting
    const D3D12_RESOURCE_DESC hudlessDesc = hudless->GetDesc();
    const Key key{in.outputSize.width, in.outputSize.height, hudlessDesc.Format, g_hdr.load(), in.depthInverted};
    if (!g_context || !(key == g_key)) {
        if (g_createFailed && key == g_key) return false;
        DestroyContext();
        g_key = key;
        g_createFailed = !CreateContext(key);
        if (g_createFailed) return false;
    }
    ComPtr<ID3D12Resource>& copy = g_hudless[g_hudlessIndex];
    if (!EnsureTexture(copy, hudlessDesc)) return false;
    CopyHudless(in.list, copy.Get(), hudless, hudlessState);

    const uint64_t frameId = g_frameId.load();
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = frameId;
    p.commandList = in.list;
    p.renderSize = {in.renderSize.width, in.renderSize.height};
    p.jitterOffset = {in.jitterX, in.jitterY};
    // Render-extent UV units -> render pixels.
    p.motionVectorScale = {in.mvScaleX * static_cast<float>(in.renderSize.width),
                           in.mvScaleY * static_cast<float>(in.renderSize.height)};
    p.frameTimeDelta = in.frameTimeMs;
    p.reset = in.reset;
    // Swapped for inverted depth, as for the upscaler (fsr.cpp).
    p.cameraNear = in.depthInverted ? in.cameraFar : in.cameraNear;
    p.cameraFar = in.depthInverted ? in.cameraNear : in.cameraFar;
    p.cameraFovAngleVertical = in.fovY;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = ffxApiGetResourceDX12(in.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    p.motionVectors = ffxApiGetResourceDX12(in.motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    if (in.hasView) CameraVectors(in.view, p.cameraPosition, p.cameraUp, p.cameraRight, p.cameraForward);
    if (const ffxReturnCode_t r = ffx::Api().dispatch(&g_context, &p.header); r != FFX_API_RETURN_OK) {
        log::Error("FSR frame generation: prepare failed: {}", ffx::ReturnCode(r));
        return false;
    }
    g_generationRect = in.outputRect;
    if (const ffxReturnCode_t r = Configure(g_enabled, copy.Get(), frameId); r != FFX_API_RETURN_OK) {
        log::Error("FSR frame generation: configure failed: {}", ffx::ReturnCode(r));
        return false;
    }
    g_taggedFrame = frameId;
    g_hudlessIndex ^= 1;
    return true;
}

void BeforePresent(bool gameFrame) {
    if (!g_swapChain) return;
    std::lock_guard lock(g_mutex);
    if (!g_context) return;
    const uint64_t frameId = g_frameId.load();
    const bool on = g_enabled && gameFrame && g_taggedFrame == frameId;
    // A frame without inputs (menu, loading screen) must not be interpolated from stale ones.
    if (!on) Configure(false, nullptr, frameId);
    if (on != g_active) log::Info("FSR frame generation {}", on ? "ON (2x)" : "off");
    g_active = on;
}

void AfterPresent() {
    if (g_swapChain) ++g_frameId;
}

void OnBeforeResize() {
    if (!g_swapChain) return;
    std::lock_guard lock(g_mutex);
    DestroyContext();  // recreated for the new size by the next Tag
}

void OnColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    g_hdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

void Detach() {
    std::lock_guard lock(g_mutex);
    if (!g_swapChain) return;
    DestroyContext();
    // Not the swapchain itself: the caller releases the last reference afterwards (FidelityFX's
    // sample does the same).
    if (g_swapChainContext) ffx::Api().destroyContext(&g_swapChainContext, nullptr);
    g_swapChainContext = nullptr;
    g_swapChain = nullptr;
    g_createFailed = false;
    g_taggedFrame = 0;
    log::Info("FSR frame generation: detached from the swapchain");
}

framegen::Status GetStatus() {
    std::lock_guard lock(g_mutex);
    framegen::Status s;
    s.backend = Backend::Fsr;
    s.available = ffx::Load();
    s.attached = g_swapChain != nullptr;
    s.enabled = g_enabled;
    s.active = g_active;
    s.framesToGenerate = 1;
    s.maxFramesToGenerate = 1;
    s.framesPresented = g_active ? 2 : 1;
    s.status = g_createFailed ? 1 : 0;
    return s;
}

}  // namespace dsru::fsr_fg

#else  // !DSRU_HAS_FIDELITYFX

namespace dsru::fsr_fg {

bool CreateSwapChain(IDXGIFactory*, ID3D12CommandQueue*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, HRESULT&) {
    return false;
}
bool CreateSwapChainForHwnd(IDXGIFactory2*, ID3D12CommandQueue*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGISwapChain1**, HRESULT&) {
    return false;
}
void Set(bool, uint32_t) {}
bool Wanted() { return false; }
bool Tag(const UpscaleInputs&, ID3D12Resource*, D3D12_RESOURCE_STATES) { return false; }
void BeforePresent(bool) {}
void AfterPresent() {}
void OnBeforeResize() {}
void OnColorSpace(DXGI_COLOR_SPACE_TYPE) {}
void Detach() {}
framegen::Status GetStatus() {
    framegen::Status s;
    s.backend = Backend::Fsr;
    return s;
}

}  // namespace dsru::fsr_fg

#endif
