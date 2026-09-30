#include "upscaler/streamline/streamline.h"

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"

#ifdef DSRU_HAS_STREAMLINE

#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_nis.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_helpers.h>
#include <sl_matrix_helpers.h>
#include <sl_security.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>

namespace dsru::streamline {

namespace {

// Streamline entry points, loaded from sl.interposer.dll.
struct Api {
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slSetD3DDevice* setD3DDevice{};
    PFun_slIsFeatureSupported* isFeatureSupported{};
    PFun_slGetFeatureVersion* getFeatureVersion{};
    PFun_slUpgradeInterface* upgradeInterface{};
    PFun_slGetNativeInterface* getNativeInterface{};
    PFun_slGetFeatureFunction* getFeatureFunction{};
    PFun_slGetNewFrameToken* getNewFrameToken{};
    PFun_slSetConstants* setConstants{};
    PFun_slSetTagForFrame* setTagForFrame{};
    PFun_slEvaluateFeature* evaluateFeature{};
    PFun_slSetFeatureLoaded* setFeatureLoaded{};
};

Api g_api;
std::once_flag g_initOnce;
bool g_initialized = false;
std::atomic<bool> g_deviceSet{false};
std::atomic<bool> g_dlssSupported{false};
std::atomic<bool> g_fgSupported{false};
std::atomic<bool> g_reflexSupported{false};
std::atomic<bool> g_pclSupported{false};
std::atomic<bool> g_nisSupported{false};  // cleared after an evaluation error
// The game's command queues and swapchain were created through Streamline's proxies: frame
// generation can run. Only when it is enabled at startup (it costs an extra present queue and copy).
std::atomic<bool> g_fgAttached{false};
bool g_fgUnloaded = false;  // present thread: sl.dlss_g unloaded after a hot switch away from it

// Frame tokens: one per presented frame, advanced after each Present.
std::mutex g_frameMutex;
uint32_t g_frameIndex = 0;
std::atomic<sl::FrameToken*> g_token{nullptr};

// Frame generation.
std::atomic<bool> g_fgUser{false};
std::atomic<uint32_t> g_fgFrames{1};
bool g_fgApplied = false;          // present thread only
uint32_t g_fgAppliedFrames = 0;    // present thread only
int g_fgSuspendFrames = 0;         // present thread only: off for a few frames (resize)
std::mutex g_fgStatusMutex;
FrameGenStatus g_fgStatus;
thread_local int t_inside = 0;

struct Inside {
    Inside() { ++t_inside; }
    ~Inside() { --t_inside; }
};

std::filesystem::path PluginDirectory() { return paths::ModuleDirectory() / "DSRUpscaler"; }

void LogCallback(sl::LogType type, const char* message) {
    std::string_view text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    switch (type) {
        case sl::LogType::eError: log::Error("[SL] {}", text); break;
        case sl::LogType::eWarn: log::Warn("[SL] {}", text); break;
        default:
            log::Debug("[SL] {}", text);
            break;
    }
}

template <class Fn>
bool Resolve(HMODULE module, const char* name, Fn*& out) {
    out = reinterpret_cast<Fn*>(GetProcAddress(module, name));
    if (!out) log::Error("Streamline: sl.interposer.dll has no export '{}'", name);
    return out != nullptr;
}

void DoInit() {
    const std::filesystem::path dir = PluginDirectory();
    const std::filesystem::path interposer = dir / "sl.interposer.dll";
    if (!std::filesystem::exists(interposer)) {
        log::Warn("Streamline: '{}' not found - DLSS unavailable", interposer.string());
        return;
    }
    // NVIDIA-signed Streamline binaries only, unless AllowUnsignedDlls (the interposer checks its
    // plugins itself).
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        if (!config::Get().allowUnsignedDlls) {
            log::Error("Streamline: signature check failed for '{}' - not loading it (AllowUnsignedDlls=0)",
                       interposer.string());
            return;
        }
        log::Warn("Streamline: signature check failed for '{}' - loading it anyway (AllowUnsignedDlls=1)",
                  interposer.string());
    }

    HMODULE module = LoadLibraryW(interposer.c_str());
    if (!module) {
        log::Error("Streamline: failed to load '{}' (error {})", interposer.string(), GetLastError());
        return;
    }
    Api api;
    if (!Resolve(module, "slInit", api.init) || !Resolve(module, "slShutdown", api.shutdown) ||
        !Resolve(module, "slSetD3DDevice", api.setD3DDevice) ||
        !Resolve(module, "slIsFeatureSupported", api.isFeatureSupported) ||
        !Resolve(module, "slGetFeatureVersion", api.getFeatureVersion) ||
        !Resolve(module, "slUpgradeInterface", api.upgradeInterface) ||
        !Resolve(module, "slGetNativeInterface", api.getNativeInterface) ||
        !Resolve(module, "slGetFeatureFunction", api.getFeatureFunction) ||
        !Resolve(module, "slGetNewFrameToken", api.getNewFrameToken) ||
        !Resolve(module, "slSetConstants", api.setConstants) ||
        !Resolve(module, "slSetTagForFrame", api.setTagForFrame) ||
        !Resolve(module, "slEvaluateFeature", api.evaluateFeature) ||
        !Resolve(module, "slSetFeatureLoaded", api.setFeatureLoaded))
        return;
    g_api = api;

    static const std::wstring pluginPath = dir.wstring();
    static const wchar_t* pluginPaths[] = {pluginPath.c_str()};
    // DLSS 5 (kFeatureDLSS_NR) is not requested: it is driven through NGX directly (upscaler/dlss_nr).
    static const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex,
                                           sl::kFeaturePCL, sl::kFeatureNIS};

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = log::Enabled(log::Level::Debug) ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;  // everything goes through LogCallback into our log
    pref.logMessageCallback = LogCallback;
    // Manual hooking: we are not the application and can't route every D3D call through
    // Streamline's proxies. We restore command list state ourselves after evaluating.
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "DSRUpscaler " DSRU_VERSION;
    pref.projectId = "b3d6f2a1-7c4e-4f19-8a2d-5e9c1b7f3a60";
    pref.renderAPI = sl::RenderAPI::eD3D12;

    Inside inside;
    const sl::Result result = g_api.init(pref, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        log::Error("Streamline: slInit failed: {}", sl::getResultAsStr(result));
        return;
    }
    g_initialized = true;
    log::Info("Streamline SDK {}.{}.{} initialized (manual hooking, plugins from '{}')", SL_VERSION_MAJOR,
              SL_VERSION_MINOR, SL_VERSION_PATCH, dir.string());
}

}  // namespace

void Init() { std::call_once(g_initOnce, DoInit); }

bool InsideStreamline() { return t_inside > 0; }

void OnDeviceCreated(ID3D12Device* device) {
    if (!g_initialized || !device || g_deviceSet.exchange(true)) return;

    Inside inside;
    if (const sl::Result r = g_api.setD3DDevice(device); r != sl::Result::eOk) {
        log::Error("Streamline: slSetD3DDevice failed: {}", sl::getResultAsStr(r));
        return;
    }

    LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    // NIS (sharpening) runs on any GPU, DLSS or not.
    if (const sl::Result nis = g_api.isFeatureSupported(sl::kFeatureNIS, adapter); nis == sl::Result::eOk) {
        g_nisSupported = true;
        log::Info("Streamline: NVIDIA Image Scaling supported");
    } else {
        log::Warn("Streamline: NVIDIA Image Scaling unavailable: {}", sl::getResultAsStr(nis));
    }
    const sl::Result supported = g_api.isFeatureSupported(sl::kFeatureDLSS, adapter);
    if (supported != sl::Result::eOk) {
        log::Warn("Streamline: DLSS not supported on this adapter: {}", sl::getResultAsStr(supported));
        return;
    }

    sl::FeatureVersion version{};
    if (g_api.getFeatureVersion(sl::kFeatureDLSS, version) == sl::Result::eOk)
        log::Info("Streamline: DLSS supported (SL {}, NGX {})", version.versionSL.toStr(), version.versionNGX.toStr());
    else
        log::Info("Streamline: DLSS supported");
    g_dlssSupported = true;

    g_reflexSupported = g_api.isFeatureSupported(sl::kFeatureReflex, adapter) == sl::Result::eOk;
    g_pclSupported = g_api.isFeatureSupported(sl::kFeaturePCL, adapter) == sl::Result::eOk;

    const sl::Result fg = g_api.isFeatureSupported(sl::kFeatureDLSS_G, adapter);
    g_fgSupported = fg == sl::Result::eOk && g_reflexSupported && g_pclSupported;
    if (g_fgSupported)
        log::Info("Streamline: DLSS Frame Generation supported");
    else
        log::Warn("Streamline: DLSS Frame Generation unavailable ({}; Reflex {}, PCL {})", sl::getResultAsStr(fg),
                  g_reflexSupported.load(), g_pclSupported.load());
}

bool UpgradeSwapChain(IDXGISwapChain1** swapChain) {
    // Without DLSS (AMD, Intel) Streamline does nothing per frame: a proxy would only be one more
    // owner of the native swapchain, which then outlives it at a hot switch and keeps the window.
    if (!g_initialized || !g_dlssSupported || !swapChain || !*swapChain) return false;
    Inside inside;
    void* proxy = *swapChain;
    const sl::Result r = g_api.upgradeInterface(&proxy);
    if (r != sl::Result::eOk || !proxy) {
        log::Error("Streamline: slUpgradeInterface(swapchain) failed: {}", sl::getResultAsStr(r));
        return false;
    }
    log::Info("Streamline: swapchain {} upgraded to proxy {}", static_cast<void*>(*swapChain), proxy);
    // The proxy AddRefs the native swapchain (sl.interposer DXGISwapChain constructor), and the
    // game now owns the proxy instead: drop the reference the game got from CreateSwapChain*, or
    // the native swapchain outlives the proxy (Streamline warns about it on release).
    IDXGISwapChain1* native = *swapChain;
    *swapChain = static_cast<IDXGISwapChain1*>(proxy);
    native->Release();
    return true;
}

// DLSS-G needs the game's command queues created through Streamline's proxy device: when it runs
// from startup, or may be switched to later (hot switch).
bool AttachFrameGen() {
    const auto& cfg = config::Get();
    return g_initialized && g_fgSupported && cfg.presenter && cfg.frameGenerationBackend == Backend::Dlss;
}

void* NativeInterface(void* proxy) {
    if (!g_initialized || !proxy) return nullptr;
    void* native = nullptr;
    if (g_api.getNativeInterface(proxy, &native) != sl::Result::eOk || !native) return nullptr;
    // Streamline adds a reference (the caller's).
    return native;
}

HRESULT CreateCommandQueue(ID3D12Device* native, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** queue,
                           bool& handled) {
    handled = false;
    if (!AttachFrameGen() || !native) return E_FAIL;
    static std::mutex mutex;
    static ID3D12Device* cachedNative = nullptr;
    static ID3D12Device* cachedProxy = nullptr;  // kept for the process lifetime
    Inside inside;
    ID3D12Device* proxy = nullptr;
    {
        std::lock_guard lock(mutex);
        if (cachedNative != native) {
            void* p = native;
            if (g_api.upgradeInterface(&p) != sl::Result::eOk || !p || p == native) {
                log::Warn("Streamline: slUpgradeInterface(device) failed, command queues are created natively");
                return E_FAIL;
            }
            cachedNative = native;
            cachedProxy = static_cast<ID3D12Device*>(p);
        }
        proxy = cachedProxy;
    }
    handled = true;
    // The proxy device returns a proxy queue; the game keeps native interfaces (manual hooking), so
    // it gets the native queue. The proxy is kept alive for the process lifetime.
    const HRESULT hr = proxy->CreateCommandQueue(desc, riid, queue);
    if (FAILED(hr) || !queue || !*queue) return hr;
    void* nativeQueue = nullptr;
    if (g_api.getNativeInterface(*queue, &nativeQueue) == sl::Result::eOk && nativeQueue && nativeQueue != *queue) {
        IUnknown* unknown = static_cast<IUnknown*>(nativeQueue);
        void* out = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(riid, &out))) *queue = out;  // the proxy's reference is kept
    }
    return hr;
}

void OnSwapChainAttached() {
    g_fgAttached = true;
    log::Info("Streamline: frame generation attached to the swapchain");
}

IDXGIFactory* ProxyFactory(IDXGIFactory* native) {
    if (!AttachFrameGen() || !native) return nullptr;
    Inside inside;
    if (g_fgUnloaded) {
        if (const sl::Result r = g_api.setFeatureLoaded(sl::kFeatureDLSS_G, true); r != sl::Result::eOk) {
            log::Error("Streamline: reloading DLSS Frame Generation failed: {}", sl::getResultAsStr(r));
            return nullptr;
        }
        g_fgUnloaded = false;
        log::Info("Streamline: DLSS Frame Generation reloaded");
    }
    void* proxy = native;
    const sl::Result r = g_api.upgradeInterface(&proxy);
    if (r != sl::Result::eOk || !proxy || proxy == native) {
        log::Warn("Streamline: slUpgradeInterface(factory) failed: {}", sl::getResultAsStr(r));
        return nullptr;
    }
    return static_cast<IDXGIFactory*>(proxy);
}

bool DlssAvailable() { return g_dlssSupported.load(); }

namespace {

sl::DLSSMode ToDlssMode(QualityMode mode) {
    switch (mode) {
        case QualityMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
        case QualityMode::Performance: return sl::DLSSMode::eMaxPerformance;
        case QualityMode::Balanced: return sl::DLSSMode::eBalanced;
        case QualityMode::Quality: return sl::DLSSMode::eMaxQuality;
        case QualityMode::UltraQuality: return sl::DLSSMode::eUltraQuality;
        case QualityMode::Native: return sl::DLSSMode::eDLAA;
    }
    return sl::DLSSMode::eDLAA;
}

sl::float4x4 Identity() {
    sl::float4x4 m;
    m[0] = {1, 0, 0, 0};
    m[1] = {0, 1, 0, 0};
    m[2] = {0, 0, 1, 0};
    m[3] = {0, 0, 0, 1};
    return m;
}

bool Check(const char* what, sl::Result r) {
    if (r == sl::Result::eOk) return true;
    log::Error("Streamline: {} failed: {}", what, sl::getResultAsStr(r));
    return false;
}

// The current frame's token (the frame being recorded / about to be presented).
sl::FrameToken* CurrentToken() {
    if (sl::FrameToken* token = g_token.load()) return token;
    std::lock_guard lock(g_frameMutex);
    if (!g_token.load()) {
        sl::FrameToken* token = nullptr;
        const uint32_t index = g_frameIndex;
        if (g_api.getNewFrameToken(token, &index) == sl::Result::eOk) g_token = token;
    }
    return g_token.load();
}

// The game's matrices are row-major for column vectors (M * v); Streamline's are row-major for row
// vectors (v * M): the transpose.
sl::float4x4 FromGame(const float* m) {
    sl::float4x4 out;
    for (uint32_t r = 0; r < 4; ++r) out[r] = {m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r]};
    return out;
}

sl::Constants MakeConstants(const UpscaleInputs& in) {
    const sl::float4x4 viewToClip = FromGame(in.projection);
    sl::float4x4 clipToView;
    sl::matrixFullInvert(clipToView, viewToClip);

    sl::Constants constants{};
    constants.cameraViewToClip = viewToClip;
    constants.clipToCameraView = clipToView;
    constants.clipToLensClip = Identity();
    constants.clipToPrevClip = Identity();
    constants.prevClipToClip = Identity();
    constants.cameraPos = {0.0f, 0.0f, 0.0f};
    constants.cameraUp = {0.0f, 1.0f, 0.0f};
    constants.cameraRight = {1.0f, 0.0f, 0.0f};
    constants.cameraFwd = {0.0f, 0.0f, 1.0f};
    if (in.hasView) {
        const sl::float4x4 worldToView = FromGame(in.view);
        sl::float4x4 viewToWorld;
        sl::matrixFullInvert(viewToWorld, worldToView);
        constants.cameraRight = {viewToWorld[0].x, viewToWorld[0].y, viewToWorld[0].z};
        constants.cameraUp = {viewToWorld[1].x, viewToWorld[1].y, viewToWorld[1].z};
        constants.cameraFwd = {viewToWorld[2].x, viewToWorld[2].y, viewToWorld[2].z};
        constants.cameraPos = {viewToWorld[3].x, viewToWorld[3].y, viewToWorld[3].z};
        if (in.hasPrevious) {
            // clip -> view -> world -> previous view -> previous clip.
            const sl::float4x4 prevWorldToView = FromGame(in.prevView);
            const sl::float4x4 prevViewToClip = FromGame(in.prevProjection);
            sl::float4x4 clipToWorld, clipToPrevView;
            sl::matrixMul(clipToWorld, clipToView, viewToWorld);
            sl::matrixMul(clipToPrevView, clipToWorld, prevWorldToView);
            sl::matrixMul(constants.clipToPrevClip, clipToPrevView, prevViewToClip);
            sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);
        }
    }
    // DLSS takes the sample's offset in the pixel, the opposite of the projection's shift (verified
    // in game: the other sign blurs the upscaling modes).
    constants.jitterOffset = {-in.jitterX, -in.jitterY};
    constants.mvecScale = {in.mvScaleX, in.mvScaleY};
    constants.cameraPinholeOffset = {0.0f, 0.0f};
    constants.cameraNear = in.cameraNear;
    constants.cameraFar = in.cameraFar;
    constants.cameraFOV = in.fovY;
    constants.cameraAspectRatio = in.aspect;
    constants.depthInverted = in.depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    return constants;
}

// The frame's camera constants: Streamline refuses different constants twice in one frame (DLSS
// and frame generation both need them), so the first caller of a frame sets them.
// Called from the game's render threads (the TAA resolve and the final blend may be recorded on
// different ones).
std::mutex g_constantsMutex;
sl::FrameToken* g_constantsToken = nullptr;  // g_constantsMutex
bool SetConstantsOnce(const UpscaleInputs& in, sl::FrameToken& token) {
    std::lock_guard lock(g_constantsMutex);
    if (g_constantsToken == &token) return true;
    if (!Check("slSetConstants", g_api.setConstants(MakeConstants(in), token, sl::ViewportHandle{0}))) return false;
    g_constantsToken = &token;
    return true;
}

void Marker(sl::PCLMarker marker, const sl::FrameToken& token) {
    if (g_pclSupported) slPCLSetMarker(marker, token);
}

// Present thread. Turns frame generation (and Reflex, which it requires) on or off.
void ApplyFrameGen(bool on, uint32_t frames) {
    if (on == g_fgApplied && (!on || frames == g_fgAppliedFrames)) return;
    if (g_reflexSupported) {
        sl::ReflexOptions reflex{};
        reflex.mode = on ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
        Check("slReflexSetOptions", slReflexSetOptions(reflex));
    }
    sl::DLSSGOptions options{};
    options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = frames;
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (!Check("slDLSSGSetOptions", slDLSSGSetOptions(sl::ViewportHandle{0}, options))) return;
    g_fgApplied = on;
    g_fgAppliedFrames = frames;
    log::Info("Streamline: frame generation {}", on ? std::format("ON ({}x)", frames + 1) : std::string("off"));
    std::lock_guard lock(g_fgStatusMutex);
    g_fgStatus.active = on;
}

}  // namespace

bool DlssOptimalRenderSize(Resolution output, QualityMode mode, Resolution& render) {
    if (!DlssAvailable()) return false;
    sl::DLSSOptions options{};
    options.mode = ToDlssMode(mode);
    options.outputWidth = output.width;
    options.outputHeight = output.height;
    sl::DLSSOptimalSettings settings{};
    Inside inside;
    if (const sl::Result r = slDLSSGetOptimalSettings(options, settings); r != sl::Result::eOk) {
        log::Warn("Streamline: slDLSSGetOptimalSettings failed: {}", sl::getResultAsStr(r));
        return false;
    }
    render = {settings.optimalRenderWidth, settings.optimalRenderHeight};
    return true;
}

void Shutdown() {
    if (!g_initialized) return;
    g_dlssSupported = false;
    g_initialized = false;
    Inside inside;
    const sl::Result r = g_api.shutdown();
    if (r == sl::Result::eOk)
        log::Info("Streamline: shut down");
    else
        log::Warn("Streamline: slShutdown failed: {}", sl::getResultAsStr(r));
}

bool EvaluateDlss(const UpscaleInputs& in, uint32_t preset) {
    if (!DlssAvailable()) return false;
    Inside inside;
    const sl::ViewportHandle viewport{0};

    sl::FrameToken* token = CurrentToken();
    if (!token) {
        log::Error("Streamline: no frame token");
        return false;
    }
    if (!SetConstantsOnce(in, *token)) return false;

    sl::DLSSOptions options{};
    options.mode = ToDlssMode(in.quality);
    options.outputWidth = in.outputSize.width;
    options.outputHeight = in.outputSize.height;
    options.colorBuffersHDR = in.colorHdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    options.useAutoExposure = in.colorHdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    const auto slPreset = static_cast<sl::DLSSPreset>(preset < static_cast<uint32_t>(sl::DLSSPreset::eCount) ? preset : 0);
    options.dlaaPreset = options.qualityPreset = options.balancedPreset = options.performancePreset =
        options.ultraPerformancePreset = options.ultraQualityPreset = slPreset;
    if (!Check("slDLSSSetOptions", slDLSSSetOptions(viewport, options))) return false;

    sl::Resource color(sl::ResourceType::eTex2d, in.color, static_cast<uint32_t>(in.colorState));
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, static_cast<uint32_t>(in.depthState));
    depth.nativeFormat = static_cast<uint32_t>(in.depthViewFormat);
    sl::Resource mvec(sl::ResourceType::eTex2d, in.motionVectors, static_cast<uint32_t>(in.motionVectorsState));
    sl::Resource output(sl::ResourceType::eTex2d, in.output, static_cast<uint32_t>(in.outputState));
    const sl::Extent renderExtent{0, 0, in.renderSize.width, in.renderSize.height};
    const sl::Extent outputExtent{0, 0, in.outputSize.width, in.outputSize.height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate,
                        &renderExtent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate,
                        &renderExtent),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate,
                        &outputExtent),
    };
    auto* commandBuffer = reinterpret_cast<sl::CommandBuffer*>(in.list);
    if (!Check("slSetTagForFrame", g_api.setTagForFrame(*token, viewport, tags,
                                                        static_cast<uint32_t>(std::size(tags)), commandBuffer)))
        return false;

    const sl::BaseStructure* inputs[] = {&viewport};
    return Check("slEvaluateFeature(DLSS)",
                 g_api.evaluateFeature(sl::kFeatureDLSS, *token, inputs, static_cast<uint32_t>(std::size(inputs)),
                                       commandBuffer));
}

bool NisAvailable() { return g_nisSupported.load(); }

bool EvaluateNis(ID3D12GraphicsCommandList* list, ID3D12Resource* input, D3D12_RESOURCE_STATES inputState,
                 ID3D12Resource* output, D3D12_RESOURCE_STATES outputState, uint32_t width, uint32_t height,
                 float sharpness) {
    if (!NisAvailable()) return false;
    Inside inside;
    const sl::ViewportHandle viewport{1};
    sl::FrameToken* token = CurrentToken();
    bool ok = token != nullptr;
    if (!ok) log::Error("Streamline: no frame token");

    sl::NISOptions options{};
    options.mode = sl::NISMode::eSharpen;
    options.hdrMode = sl::NISHDR::eNone;  // display-referred image
    options.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    ok = ok && Check("slNISSetOptions", slNISSetOptions(viewport, options));

    sl::Resource in(sl::ResourceType::eTex2d, input, static_cast<uint32_t>(inputState));
    sl::Resource out(sl::ResourceType::eTex2d, output, static_cast<uint32_t>(outputState));
    const sl::Extent extent{0, 0, width, height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&in, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &extent),
        sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &extent),
    };
    auto* commandBuffer = reinterpret_cast<sl::CommandBuffer*>(list);
    ok = ok && Check("slSetTagForFrame(NIS)", g_api.setTagForFrame(*token, viewport, tags,
                                                                    static_cast<uint32_t>(std::size(tags)),
                                                                    commandBuffer));
    const sl::BaseStructure* inputs[] = {&viewport};
    ok = ok && Check("slEvaluateFeature(NIS)",
                     g_api.evaluateFeature(sl::kFeatureNIS, *token, inputs, static_cast<uint32_t>(std::size(inputs)),
                                           commandBuffer));
    if (!ok) {
        g_nisSupported = false;
        log::Error("Streamline: NVIDIA Image Scaling disabled after this error");
    }
    return ok;
}

bool FrameGenAvailable() { return g_fgSupported.load(); }

void DetachFrameGen() {
    if (!g_fgAttached) return;
    Inside inside;
    ApplyFrameGen(false, g_fgFrames);
    g_fgAttached = false;
    log::Info("Streamline: frame generation detached from the swapchain");
}

void UnloadFrameGen() {
    if (!g_initialized || !g_fgSupported || g_fgUnloaded) return;
    Inside inside;
    // sl.dlss_g keeps references to the native swapchain until it is unloaded (NVIDIA's DLSS-G guide,
    // "how to avoid unnecessary overhead": release the swapchain, then unload).
    if (const sl::Result r = g_api.setFeatureLoaded(sl::kFeatureDLSS_G, false); r != sl::Result::eOk) {
        log::Warn("Streamline: unloading DLSS Frame Generation failed: {}", sl::getResultAsStr(r));
        return;
    }
    g_fgUnloaded = true;
    log::Info("Streamline: DLSS Frame Generation unloaded");
}

bool FrameGenAttached() { return g_fgAttached.load(); }

void SetFrameGen(bool enabled, uint32_t framesToGenerate) {
    g_fgUser = enabled;
    g_fgFrames = std::clamp(framesToGenerate, 1u, 3u);
    std::lock_guard lock(g_fgStatusMutex);
    g_fgStatus.enabled = enabled;
    g_fgStatus.framesToGenerate = g_fgFrames;
}

bool FrameGenWanted() { return g_fgUser && g_fgAttached; }

FrameGenStatus GetFrameGenStatus() {
    std::lock_guard lock(g_fgStatusMutex);
    FrameGenStatus s = g_fgStatus;
    s.available = g_fgSupported;
    s.attached = g_fgAttached;
    return s;
}

bool TagFrameGen(const UpscaleInputs& in, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState) {
    if (!FrameGenWanted()) return false;
    Inside inside;
    sl::FrameToken* token = CurrentToken();
    if (!token) return false;
    const sl::ViewportHandle viewport{0};
    if (!SetConstantsOnce(in, *token)) return false;

    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, static_cast<uint32_t>(in.depthState));
    depth.nativeFormat = static_cast<uint32_t>(in.depthViewFormat);
    sl::Resource mvec(sl::ResourceType::eTex2d, in.motionVectors, static_cast<uint32_t>(in.motionVectorsState));
    sl::Resource color(sl::ResourceType::eTex2d, hudless, static_cast<uint32_t>(hudlessState));
    const sl::Extent renderExtent{0, 0, in.renderSize.width, in.renderSize.height};
    // A letterboxed scene: DLSS-G runs on the back buffer's subrect (the bars are copied as they
    // are), and the HUD-less extent is that subrect.
    const bool subrect = in.outputRect.width && in.outputRect.height;
    const sl::Extent outputExtent = subrect ? sl::Extent{in.outputRect.y, in.outputRect.x, in.outputRect.width,
                                                         in.outputRect.height}
                                            : sl::Extent{0, 0, in.outputSize.width, in.outputSize.height};
    // Depth and motion vectors are our own copies, untouched until the next frame's copy (after
    // this Present on the GPU). The scene texture gets reused: Streamline copies it now.
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent,
                        &renderExtent),
        sl::ResourceTag(&color, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent),
        sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &outputExtent),
    };
    const uint32_t count = static_cast<uint32_t>(std::size(tags)) - (subrect ? 0u : 1u);
    return Check("slSetTagForFrame(frame generation)",
                 g_api.setTagForFrame(*token, viewport, tags, count, reinterpret_cast<sl::CommandBuffer*>(in.list)));
}

void BeforePresent(bool gameFrame) {
    if (!g_initialized || !g_dlssSupported) return;
    Inside inside;
    sl::FrameToken* token = CurrentToken();
    if (!token) return;
    if (!g_fgAttached) return;
    Marker(sl::PCLMarker::eSimulationEnd, *token);
    Marker(sl::PCLMarker::eRenderSubmitStart, *token);
    Marker(sl::PCLMarker::eRenderSubmitEnd, *token);
    if (g_fgSuspendFrames > 0) --g_fgSuspendFrames;
    ApplyFrameGen(g_fgUser && gameFrame && g_fgSuspendFrames == 0, g_fgFrames);
    Marker(sl::PCLMarker::ePresentStart, *token);
}

void AfterPresent() {
    if (!g_initialized || (!g_dlssSupported && !g_nisSupported)) return;
    Inside inside;
    const bool attached = g_fgAttached;
    if (sl::FrameToken* token = g_token.load(); token && attached) Marker(sl::PCLMarker::ePresentEnd, *token);

    sl::FrameToken* next = nullptr;
    {
        std::lock_guard lock(g_frameMutex);
        const uint32_t index = ++g_frameIndex;
        if (g_api.getNewFrameToken(next, &index) != sl::Result::eOk) next = nullptr;
        g_token = next;
    }
    if (!next || !attached) return;
    if (g_reflexSupported) slReflexSleep(*next);
    Marker(sl::PCLMarker::eSimulationStart, *next);

    // Frame generation state, a few times a second.
    static uint32_t frames = 0;
    if (g_fgApplied && ++frames % 30 == 0) {
        sl::DLSSGState state{};
        if (slDLSSGGetState(sl::ViewportHandle{0}, state, nullptr) == sl::Result::eOk) {
            std::lock_guard lock(g_fgStatusMutex);
            const bool changed = static_cast<uint32_t>(state.status) != g_fgStatus.status;
            g_fgStatus.status = static_cast<uint32_t>(state.status);
            g_fgStatus.maxFramesToGenerate = state.numFramesToGenerateMax;
            g_fgStatus.framesPresented = state.numFramesActuallyPresented;
            if (changed && state.status != sl::DLSSGStatus::eOk)
                log::Warn("Streamline: frame generation status 0x{:x}", static_cast<uint32_t>(state.status));
        }
    }
}

void OnBeforeResize() {
    if (!g_fgAttached) return;
    Inside inside;
    ApplyFrameGen(false, g_fgFrames);
    g_fgSuspendFrames = 3;
}

}  // namespace dsru::streamline

// The sl_*.h feature helpers (slDLSSGetOptimalSettings, ...) call this; normally it comes from
// linking sl.interposer.lib, but we load the DLL dynamically (after checking its signature).
extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function) {
    if (!dsru::streamline::g_api.getFeatureFunction) return sl::Result::eErrorNotInitialized;
    return dsru::streamline::g_api.getFeatureFunction(feature, functionName, function);
}

#else  // !DSRU_HAS_STREAMLINE

namespace dsru::streamline {

void Init() {}
bool InsideStreamline() { return false; }
void OnDeviceCreated(ID3D12Device*) {}
bool UpgradeSwapChain(IDXGISwapChain1**) { return false; }
IDXGIFactory* ProxyFactory(IDXGIFactory*) { return nullptr; }
bool FrameGenAttached() { return false; }
void* NativeInterface(void*) { return nullptr; }
HRESULT CreateCommandQueue(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**, bool& handled) {
    handled = false;
    return E_FAIL;
}
void OnSwapChainAttached() {}
bool DlssAvailable() { return false; }
bool DlssOptimalRenderSize(Resolution, QualityMode, Resolution&) { return false; }
bool EvaluateDlss(const UpscaleInputs&, uint32_t) { return false; }
bool NisAvailable() { return false; }
bool EvaluateNis(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, ID3D12Resource*,
                 D3D12_RESOURCE_STATES, uint32_t, uint32_t, float) {
    return false;
}
bool FrameGenAvailable() { return false; }
void DetachFrameGen() {}
void UnloadFrameGen() {}
void SetFrameGen(bool, uint32_t) {}
bool FrameGenWanted() { return false; }
FrameGenStatus GetFrameGenStatus() { return {}; }
bool TagFrameGen(const UpscaleInputs&, ID3D12Resource*, D3D12_RESOURCE_STATES) { return false; }
void BeforePresent(bool) {}
void AfterPresent() {}
void OnBeforeResize() {}
void Shutdown() {}

}  // namespace dsru::streamline

#endif
