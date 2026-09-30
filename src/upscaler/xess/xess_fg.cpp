#include "upscaler/xess/xess_fg.h"

#include "core/log.h"

#ifdef DSRU_HAS_XESS

#include "upscaler/signature.h"

#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell.h>
#include <xell/xell_d3d12.h>

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace dsru::xefg {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct Api {
    decltype(&xefgSwapChainGetVersion) getVersion{};
    decltype(&xefgSwapChainD3D12CreateContext) createContext{};
    decltype(&xefgSwapChainSetLatencyReduction) setLatencyReduction{};
    decltype(&xefgSwapChainSetLoggingCallback) setLoggingCallback{};
    decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) initFromSwapChainDesc{};
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) getSwapChainPtr{};
    decltype(&xefgSwapChainGetProperties) getProperties{};
    decltype(&xefgSwapChainSetEnabled) setEnabled{};
    decltype(&xefgSwapChainSetPresentId) setPresentId{};
    decltype(&xefgSwapChainSetNumInterpolatedFrames) setNumInterpolatedFrames{};
    decltype(&xefgSwapChainD3D12TagFrameResource) tagFrameResource{};
    decltype(&xefgSwapChainTagFrameConstants) tagFrameConstants{};
    decltype(&xefgSwapChainGetLastPresentStatus) getLastPresentStatus{};
    decltype(&xefgSwapChainDestroy) destroy{};
    decltype(&xellDestroyContext) xellDestroyContext{};
    decltype(&xellD3D12CreateContext) xellCreateContext{};
    decltype(&xellSetSleepMode) xellSetSleepMode{};
    decltype(&xellSleep) xellSleep{};
    decltype(&xellAddMarkerData) xellAddMarkerData{};
};

Api g_api;
std::mutex g_mutex;  // Tag (render threads) against the present thread

xefg_swapchain_handle_t g_context = nullptr;
xell_context_handle_t g_xell = nullptr;
DXGI_FORMAT g_backBufferFormat = DXGI_FORMAT_UNKNOWN;
std::string g_versionName;
uint32_t g_maxFrames = 1;

std::atomic<uint32_t> g_frameId{1};  // XeLL frame id = XeSS-FG present id; +1 per Present
uint32_t g_taggedFrame = 0;          // present id of the last Tag
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_detaching{false};  // no tagging while the proxy swapchain goes away
std::atomic<uint32_t> g_frames{1};
bool g_lowLatency = false;      // XeLL sleep mode applied (present thread)
bool g_active = false;          // frame generation enabled in XeSS-FG (present thread)
uint32_t g_appliedFrames = 0;   // interpolated frames applied (present thread)
uint32_t g_framesPresented = 0;
int32_t g_lastResult = 0;

const char* ResultName(xefg_swapchain_result_t r) {
    switch (r) {
        case XEFG_SWAPCHAIN_RESULT_SUCCESS: return "success";
        case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER: return "warning: old driver";
        case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES: return "warning: too few frames";
        case XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH: return "warning: frame id mismatch";
        case XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS: return "warning: missing present status";
        case XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH: return "warning: resource sizes mismatch";
        case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device";
        case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
        case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of device memory";
        case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE: return "device error";
        case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED: return "unsupported";
        case XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY: return "can't load library";
        case XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES: return "mismatched input resources";
        case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_OUTPUT_RESOURCES: return "incorrect output resources";
        case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES: return "incorrect input resources";
        case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED: return "XeLL unsupported";
        case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING: return "XeLL function missing";
        case XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE: return "HRESULT failure";
        case XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL: return "DXGI invalid call";
        case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong call order";
        default: return "error";
    }
}

void LogCallback(const char* message, xefg_swapchain_logging_level_t level, void*) {
    std::string_view text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    switch (level) {
        case XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR: log::Error("[XeSS-FG] {}", text); break;
        case XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING: log::Warn("[XeSS-FG] {}", text); break;
        default: log::Debug("[XeSS-FG] {}", text); break;
    }
}

template <class Fn>
bool Resolve(HMODULE module, const char* dll, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (!out) log::Error("XeSS-FG: {} has no export '{}'", dll, name);
    return out != nullptr;
}

bool Load() {
    static std::once_flag once;
    static bool loaded = false;
    std::call_once(once, [] {
        auto* xell = static_cast<HMODULE>(upscaler::LoadSignedModule("XeSS-FG", L"libxell.dll"));
        auto* fg = xell ? static_cast<HMODULE>(upscaler::LoadSignedModule("XeSS-FG", L"libxess_fg.dll")) : nullptr;
        if (!fg) return;
        Api api;
        constexpr const char* kFg = "libxess_fg.dll";
        constexpr const char* kXell = "libxell.dll";
        if (!Resolve(fg, kFg, "xefgSwapChainGetVersion", api.getVersion) ||
            !Resolve(fg, kFg, "xefgSwapChainD3D12CreateContext", api.createContext) ||
            !Resolve(fg, kFg, "xefgSwapChainSetLatencyReduction", api.setLatencyReduction) ||
            !Resolve(fg, kFg, "xefgSwapChainSetLoggingCallback", api.setLoggingCallback) ||
            !Resolve(fg, kFg, "xefgSwapChainD3D12InitFromSwapChainDesc", api.initFromSwapChainDesc) ||
            !Resolve(fg, kFg, "xefgSwapChainD3D12GetSwapChainPtr", api.getSwapChainPtr) ||
            !Resolve(fg, kFg, "xefgSwapChainGetProperties", api.getProperties) ||
            !Resolve(fg, kFg, "xefgSwapChainSetEnabled", api.setEnabled) ||
            !Resolve(fg, kFg, "xefgSwapChainSetPresentId", api.setPresentId) ||
            !Resolve(fg, kFg, "xefgSwapChainSetNumInterpolatedFrames", api.setNumInterpolatedFrames) ||
            !Resolve(fg, kFg, "xefgSwapChainD3D12TagFrameResource", api.tagFrameResource) ||
            !Resolve(fg, kFg, "xefgSwapChainTagFrameConstants", api.tagFrameConstants) ||
            !Resolve(fg, kFg, "xefgSwapChainGetLastPresentStatus", api.getLastPresentStatus) ||
            !Resolve(fg, kFg, "xefgSwapChainDestroy", api.destroy) ||
            !Resolve(xell, kXell, "xellDestroyContext", api.xellDestroyContext) ||
            !Resolve(xell, kXell, "xellD3D12CreateContext", api.xellCreateContext) ||
            !Resolve(xell, kXell, "xellSetSleepMode", api.xellSetSleepMode) ||
            !Resolve(xell, kXell, "xellSleep", api.xellSleep) ||
            !Resolve(xell, kXell, "xellAddMarkerData", api.xellAddMarkerData))
            return;
        g_api = api;
        xefg_swapchain_version_t v{};
        g_api.getVersion(&v);
        g_versionName = std::format("XeSS-FG {}.{}.{}", v.major, v.minor, v.patch);
        loaded = true;
    });
    return loaded;
}

// Creates XeLL and the XeSS-FG proxy swapchain for `hwnd` / `desc`. Called with g_mutex held.
bool Create(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, REFIID riid, void** out) {
    if (!Load()) return false;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGIFactory2> factory2;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
        return false;

    if (const xell_result_t r = g_api.xellCreateContext(device.Get(), &g_xell); r != XELL_RESULT_SUCCESS) {
        log::Warn("XeSS-FG: XeLL unavailable ({}) - creating the game's swapchain normally", static_cast<int>(r));
        g_xell = nullptr;
        return false;
    }
    if (const xefg_swapchain_result_t r = g_api.createContext(device.Get(), &g_context);
        r < XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        log::Warn("{}: not supported on this GPU ({}) - creating the game's swapchain normally", g_versionName,
                  ResultName(r));
        g_context = nullptr;
        return false;
    }
    g_api.setLoggingCallback(g_context, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, LogCallback, nullptr);
    g_api.setLatencyReduction(g_context, g_xell);

    xefg_swapchain_d3d12_init_params_t params{};
    params.initFlags = 0;  // Dark Souls Remastered: standard depth (1 = far), not inverted
    params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    params.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
    if (const xefg_swapchain_result_t r = g_api.initFromSwapChainDesc(g_context, hwnd, &desc, fullscreenDesc,
                                                                       queue, factory2.Get(), &params);
        r < XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        log::Error("{}: swapchain creation failed ({}) - creating the game's swapchain normally", g_versionName,
                   ResultName(r));
        g_context = nullptr;  // leaked: destroying it now could take the half-made swapchain down with it
        return false;
    }
    if (g_api.getSwapChainPtr(g_context, riid, out) < XEFG_SWAPCHAIN_RESULT_SUCCESS || !*out) {
        log::Error("{}: no proxy swapchain", g_versionName);
        g_context = nullptr;
        return false;
    }
    xefg_swapchain_properties_t props{};
    if (g_api.getProperties(g_context, &props) >= XEFG_SWAPCHAIN_RESULT_SUCCESS && props.maxSupportedInterpolations)
        g_maxFrames = std::min(props.maxSupportedInterpolations, 3u);
    g_backBufferFormat = desc.Format;
    log::Info("{}: the game's swapchain is XeSS-FG's proxy ({}x{}, up to {} generated frames)", g_versionName,
              desc.Width, desc.Height, g_maxFrames);
    return true;
}

void Marker(xell_latency_marker_type_t marker, uint32_t frameId) {
    if (g_xell) g_api.xellAddMarkerData(g_xell, frameId, marker);
}

xefg_swapchain_d3d12_resource_data_t Resource(xefg_swapchain_resource_type_t type,
                                              xefg_swapchain_resource_validity_t validity, ID3D12Resource* r,
                                              D3D12_RESOURCE_STATES state, Resolution size) {
    xefg_swapchain_d3d12_resource_data_t data{};
    data.type = type;
    data.validity = validity;
    data.resourceBase = {0, 0};
    data.resourceSize = {size.width, size.height};
    data.pResource = r;
    data.incomingState = state;
    return data;
}

// The game's matrices are row-major for column vectors (M * v); XeSS-FG takes DirectXMath's
// row-vector convention (v * M): the transpose.
void Transposed(const float* m, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[c * 4 + r];
}

}  // namespace

bool CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, DXGI_SWAP_CHAIN_DESC* desc,
                     IDXGISwapChain** out, HRESULT& result) {
    if (!desc) return false;
    std::lock_guard lock(g_mutex);
    DXGI_SWAP_CHAIN_DESC1 desc1{};
    desc1.Width = desc->BufferDesc.Width;
    desc1.Height = desc->BufferDesc.Height;
    desc1.Format = desc->BufferDesc.Format;
    desc1.SampleDesc = desc->SampleDesc;
    desc1.BufferUsage = desc->BufferUsage;
    desc1.BufferCount = desc->BufferCount;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.SwapEffect = desc->SwapEffect;
    desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Flags = desc->Flags;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
    fullscreen.RefreshRate = desc->BufferDesc.RefreshRate;
    fullscreen.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreen.Scaling = desc->BufferDesc.Scaling;
    fullscreen.Windowed = desc->Windowed;
    if (!Create(factory, queue, desc->OutputWindow, desc1, &fullscreen, IID_PPV_ARGS(out))) return false;
    result = S_OK;
    return true;
}

bool CreateSwapChainForHwnd(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd,
                            const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                            IDXGISwapChain1** out, HRESULT& result) {
    if (!desc) return false;
    std::lock_guard lock(g_mutex);
    if (!Create(factory, queue, hwnd, *desc, fullscreenDesc, IID_PPV_ARGS(out))) return false;
    result = S_OK;
    return true;
}

void Set(bool enabled, uint32_t framesToGenerate) {
    g_enabled = enabled;
    g_frames = std::clamp(framesToGenerate, 1u, 3u);
}

bool Wanted() { return g_enabled && g_context && !g_detaching; }

bool Tag(const UpscaleInputs& in, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState) {
    if (!Wanted()) return false;
    std::lock_guard lock(g_mutex);
    if (!g_context || g_detaching) return false;  // detached while waiting
    const uint32_t presentId = g_frameId.load();
    // Depth and motion vectors are our own copies, untouched until the next frame's copy (after this
    // Present on the GPU). The HUD-less scene gets reused: XeSS-FG copies it now, and only when it
    // has the back buffer's format (otherwise the HUD is interpolated with the scene).
    const xefg_swapchain_d3d12_resource_data_t resources[] = {
        Resource(XEFG_SWAPCHAIN_RES_DEPTH, XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT, in.depth, in.depthState, in.renderSize),
        Resource(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT, in.motionVectors,
                 in.motionVectorsState, in.renderSize),
        Resource(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, XEFG_SWAPCHAIN_RV_ONLY_NOW, hudless, hudlessState, in.outputSize),
    };
    const bool withHudless = hudless && hudless->GetDesc().Format == g_backBufferFormat;
    xefg_swapchain_d3d12_resource_data_t hudlessData = resources[2];
    // A letterboxed scene: the HUD-less image's valid region, and the back buffer's (no resource:
    // the region only) so that only it is interpolated.
    const bool subrect = in.outputRect.width && in.outputRect.height;
    if (subrect) {
        hudlessData.resourceBase = {in.outputRect.x, in.outputRect.y};
        hudlessData.resourceSize = {in.outputRect.width, in.outputRect.height};
    }
    for (size_t i = 0; i < (withHudless ? 3u : 2u); ++i) {
        const xefg_swapchain_d3d12_resource_data_t& data = i == 2 ? hudlessData : resources[i];
        if (const xefg_swapchain_result_t r = g_api.tagFrameResource(g_context, in.list, presentId, &data);
            r < XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            log::Error("XeSS-FG: tagging resource {} failed: {}", static_cast<int>(data.type), ResultName(r));
            return false;
        }
    }
    if (subrect) {
        xefg_swapchain_d3d12_resource_data_t backBuffer =
            Resource(XEFG_SWAPCHAIN_RES_BACKBUFFER, XEFG_SWAPCHAIN_RV_ONLY_NOW, nullptr, D3D12_RESOURCE_STATE_COMMON,
                     {in.outputRect.width, in.outputRect.height});
        backBuffer.resourceBase = {in.outputRect.x, in.outputRect.y};
        static bool warned = false;
        if (const xefg_swapchain_result_t r = g_api.tagFrameResource(g_context, in.list, presentId, &backBuffer);
            r < XEFG_SWAPCHAIN_RESULT_SUCCESS && !warned) {
            warned = true;
            log::Warn("XeSS-FG: tagging the back buffer's letterbox region failed: {} - the whole back buffer is "
                      "interpolated",
                      ResultName(r));
        }
    }
    xefg_swapchain_frame_constant_data_t constants{};
    if (in.hasView) Transposed(in.view, constants.viewMatrix);
    Transposed(in.projection, constants.projectionMatrix);
    constants.jitterOffsetX = in.jitterX;
    constants.jitterOffsetY = in.jitterY;
    // Render-extent UV units -> render pixels.
    constants.motionVectorScaleX = in.mvScaleX * static_cast<float>(in.renderSize.width);
    constants.motionVectorScaleY = in.mvScaleY * static_cast<float>(in.renderSize.height);
    constants.resetHistory = in.reset ? 1u : 0u;
    constants.frameRenderTime = in.frameTimeMs;
    if (const xefg_swapchain_result_t r = g_api.tagFrameConstants(g_context, presentId, &constants);
        r < XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        log::Error("XeSS-FG: tagging constants failed: {}", ResultName(r));
        return false;
    }
    g_taggedFrame = presentId;
    return true;
}

void BeforePresent(bool gameFrame) {
    if (!g_context) return;
    std::lock_guard lock(g_mutex);
    const uint32_t id = g_frameId.load();
    Marker(XELL_SIMULATION_END, id);
    Marker(XELL_RENDERSUBMIT_START, id);
    Marker(XELL_RENDERSUBMIT_END, id);

    // XeLL's low latency mode stays on while the user has frame generation on (XeSS-FG requires it
    // before being enabled); XeSS-FG itself is off for frames without inputs.
    const bool user = g_enabled;
    if (user != g_lowLatency) {
        xell_sleep_params_t sleep{};
        sleep.bLowLatencyMode = user ? 1u : 0u;
        g_api.xellSetSleepMode(g_xell, &sleep);
        g_lowLatency = user;
    }
    const uint32_t frames = std::min(g_frames.load(), g_maxFrames);
    if (user && frames != g_appliedFrames) {
        g_api.setNumInterpolatedFrames(g_context, frames);
        g_appliedFrames = frames;
    }
    const bool on = user && gameFrame && g_taggedFrame == id;
    if (on != g_active) {
        g_api.setEnabled(g_context, on ? 1u : 0u);
        log::Info("XeSS-FG {}", on ? std::format("ON ({}x)", frames + 1) : std::string("off"));
        g_active = on;
    }
    Marker(XELL_PRESENT_START, id);
    g_api.setPresentId(g_context, id);
}

void AfterPresent() {
    if (!g_context) return;
    std::lock_guard lock(g_mutex);
    Marker(XELL_PRESENT_END, g_frameId.load());
    const uint32_t next = ++g_frameId;

    static uint32_t count = 0;
    if (g_active && ++count % 30 == 0) {
        xefg_swapchain_present_status_t status{};
        if (g_api.getLastPresentStatus(g_context, &status) >= XEFG_SWAPCHAIN_RESULT_SUCCESS) {
            g_framesPresented = status.framesPresented;
            if (status.frameGenResult != g_lastResult && status.frameGenResult < XEFG_SWAPCHAIN_RESULT_SUCCESS)
                log::Warn("XeSS-FG: frame generation reports {}", ResultName(status.frameGenResult));
            g_lastResult = status.frameGenResult;
        }
    }
    if (g_lowLatency) g_api.xellSleep(g_xell, next);
    Marker(XELL_SIMULATION_START, next);
}

void DetachBeforeRelease() {
    std::lock_guard lock(g_mutex);
    if (!g_context) return;
    g_detaching = true;
    if (g_active) g_api.setEnabled(g_context, 0u);
    g_active = false;
}

void DetachAfterRelease() {
    std::lock_guard lock(g_mutex);
    if (!g_context) return;
    // XeLL must outlive the XeSS-FG context.
    if (const xefg_swapchain_result_t r = g_api.destroy(g_context); r < XEFG_SWAPCHAIN_RESULT_SUCCESS)
        log::Error("XeSS-FG: context destruction failed ({}) - leaked", ResultName(r));
    else if (g_xell)
        g_api.xellDestroyContext(g_xell);
    g_context = nullptr;
    g_xell = nullptr;
    g_lowLatency = false;
    g_appliedFrames = 0;
    g_taggedFrame = 0;
    g_detaching = false;
    log::Info("XeSS-FG: detached from the swapchain");
}

framegen::Status GetStatus() {
    std::lock_guard lock(g_mutex);
    framegen::Status s;
    s.backend = Backend::Xess;
    s.available = Load();
    s.attached = g_context != nullptr;
    s.enabled = g_enabled;
    s.active = g_active;
    s.framesToGenerate = g_frames;
    s.maxFramesToGenerate = g_context ? g_maxFrames : 0;
    s.framesPresented = g_active ? (g_framesPresented ? g_framesPresented : g_frames + 1) : 1;
    s.status = g_lastResult < 0 ? static_cast<uint32_t>(-g_lastResult) : 0;
    return s;
}

}  // namespace dsru::xefg

#else  // !DSRU_HAS_XESS

namespace dsru::xefg {

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
framegen::Status GetStatus() {
    framegen::Status s;
    s.backend = Backend::Xess;
    return s;
}
void DetachBeforeRelease() {}
void DetachAfterRelease() {}

}  // namespace dsru::xefg

#endif
