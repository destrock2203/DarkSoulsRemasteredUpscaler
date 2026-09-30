#include "upscaler/xess/xess.h"

#include "core/log.h"

#ifdef DSRU_HAS_XESS

#include "upscaler/signature.h"

#include <xess/xess.h>
#include <xess/xess_d3d12.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <vector>

namespace dsru::xess {

namespace {

struct Api {
    decltype(&xessGetVersion) getVersion{};
    decltype(&xessD3D12CreateContext) createContext{};
    decltype(&xessD3D12Init) init{};
    decltype(&xessD3D12Execute) execute{};
    decltype(&xessDestroyContext) destroyContext{};
    decltype(&xessSetVelocityScale) setVelocityScale{};
    decltype(&xessSetLoggingCallback) setLoggingCallback{};
    decltype(&xessIsOptimalDriver) isOptimalDriver{};
    decltype(&xessForceLegacyScaleFactors) forceLegacyScaleFactors{};
};

Api g_api;
ID3D12Device* g_device = nullptr;
std::once_flag g_loadOnce;
std::atomic<bool> g_available{false};
std::string g_versionName;
xess_context_handle_t g_spare = nullptr;  // created by the support check, used by the first Init

struct Key {
    uint32_t width = 0, height = 0;
    xess_quality_settings_t quality = XESS_QUALITY_SETTING_AA;
    bool hdr = false;
    bool invertedDepth = false;
    bool operator==(const Key&) const = default;
};
xess_context_handle_t g_context = nullptr;
Key g_key;
bool g_failed = false;  // Init failed for g_key: don't retry every frame
float g_velocityScale[2] = {0.0f, 0.0f};

// Contexts replaced while command lists recorded with them may still be in flight (XeSS requires
// its pending work to complete before a context is re-initialized or destroyed).
struct Retired {
    xess_context_handle_t context;
    uint64_t present;
};
std::vector<Retired> g_retired;
uint64_t g_presentCount = 0;
constexpr uint64_t kRetireFrames = 8;

const char* ResultName(xess_result_t r) {
    switch (r) {
        case XESS_RESULT_SUCCESS: return "success";
        case XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "warning: nonexistent folder";
        case XESS_RESULT_WARNING_OLD_DRIVER: return "warning: old driver";
        case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device";
        case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
        case XESS_RESULT_ERROR_UNINITIALIZED: return "uninitialized";
        case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of device memory";
        case XESS_RESULT_ERROR_DEVICE: return "device error";
        case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
        case XESS_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
        case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "operation in progress";
        case XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
        case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "can't load library";
        case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong call order";
        default: return "unknown error";
    }
}

void LogCallback(const char* message, xess_logging_level_t level) {
    std::string_view text = message ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    switch (level) {
        case XESS_LOGGING_LEVEL_ERROR: log::Error("[XeSS] {}", text); break;
        case XESS_LOGGING_LEVEL_WARNING: log::Warn("[XeSS] {}", text); break;
        default: log::Debug("[XeSS] {}", text); break;
    }
}

template <class Fn>
bool Resolve(HMODULE module, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (!out) log::Error("XeSS: libxess.dll has no export '{}'", name);
    return out != nullptr;
}

xess_context_handle_t NewContext() {
    xess_context_handle_t context = nullptr;
    const xess_result_t r = g_api.createContext(g_device, &context);
    if (r < XESS_RESULT_SUCCESS || !context) {
        log::Warn("XeSS: context creation failed: {}", ResultName(r));
        return nullptr;
    }
    g_api.setLoggingCallback(context, XESS_LOGGING_LEVEL_WARNING, LogCallback);
    return context;
}

void DoLoad() {
    if (!g_device) {
        log::Warn("XeSS: no D3D12 device yet");
        return;
    }
    auto* module = static_cast<HMODULE>(upscaler::LoadSignedModule("XeSS", L"libxess.dll"));
    if (!module) return;
    Api api;
    if (!Resolve(module, "xessGetVersion", api.getVersion) ||
        !Resolve(module, "xessD3D12CreateContext", api.createContext) ||
        !Resolve(module, "xessD3D12Init", api.init) || !Resolve(module, "xessD3D12Execute", api.execute) ||
        !Resolve(module, "xessDestroyContext", api.destroyContext) ||
        !Resolve(module, "xessSetVelocityScale", api.setVelocityScale) ||
        !Resolve(module, "xessSetLoggingCallback", api.setLoggingCallback) ||
        !Resolve(module, "xessIsOptimalDriver", api.isOptimalDriver) ||
        !Resolve(module, "xessForceLegacyScaleFactors", api.forceLegacyScaleFactors))
        return;
    g_api = api;

    xess_version_t version{};
    g_api.getVersion(&version);
    g_versionName = std::format("XeSS {}.{}.{}", version.major, version.minor, version.patch);

    // A context only gets created on supported devices (SM 6.4 + DP4a, or Intel XMX).
    g_spare = NewContext();
    if (!g_spare) {
        log::Warn("{}: not supported on this GPU", g_versionName);
        return;
    }
    if (g_api.isOptimalDriver(g_spare) == XESS_RESULT_WARNING_OLD_DRIVER)
        log::Warn("{}: the GPU driver is older than recommended", g_versionName);
    log::Info("{} loaded", g_versionName);
    g_available = true;
}

xess_quality_settings_t ToXess(QualityMode mode) {
    switch (mode) {
        case QualityMode::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
        case QualityMode::Performance: return XESS_QUALITY_SETTING_PERFORMANCE;
        case QualityMode::Balanced: return XESS_QUALITY_SETTING_BALANCED;
        case QualityMode::Quality: return XESS_QUALITY_SETTING_QUALITY;
        case QualityMode::UltraQuality: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
        case QualityMode::Native: return XESS_QUALITY_SETTING_AA;
    }
    return XESS_QUALITY_SETTING_AA;
}

bool InitContext(const Key& key) {
    xess_context_handle_t context = g_spare ? g_spare : NewContext();
    g_spare = nullptr;
    if (!context) return false;

    xess_d3d12_init_params_t params{};
    params.outputResolution = {key.width, key.height};
    params.qualitySetting = key.quality;
    params.initFlags = key.invertedDepth ? XESS_INIT_FLAG_INVERTED_DEPTH : 0u;
    // The game's HDR scene (native AA) gets auto exposure; the tonemapped scene (upscaling) is LDR.
    params.initFlags |= key.hdr ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : XESS_INIT_FLAG_LDR_INPUT_COLOR;
    // The pre-1.3 factors (Quality 1.5x, like DLSS and FSR, instead of 1.7x): NominalScale matches.
    g_api.forceLegacyScaleFactors(context, true);
    const xess_result_t r = g_api.init(context, &params);
    if (r < XESS_RESULT_SUCCESS) {
        log::Error("XeSS: init for {}x{} failed: {}", key.width, key.height, ResultName(r));
        g_retired.push_back({context, g_presentCount});
        return false;
    }
    g_context = context;
    g_velocityScale[0] = g_velocityScale[1] = 0.0f;
    log::Info("{}: context for {}x{} (quality setting {}, {})", g_versionName, key.width, key.height,
              static_cast<int>(key.quality), key.hdr ? "HDR" : "LDR");
    return true;
}

}  // namespace

void OnDeviceCreated(ID3D12Device* device) {
    if (!g_device) g_device = device;
}

void Load() { std::call_once(g_loadOnce, DoLoad); }

bool Available() { return g_available.load(); }

std::string VersionName() { return g_versionName; }

bool Evaluate(const UpscaleInputs& in) {
    if (!g_available) return false;
    const Key key{in.outputSize.width, in.outputSize.height, ToXess(in.quality), in.colorHdr, in.depthInverted};
    if (!g_context || !(key == g_key)) {
        if (g_failed && key == g_key) return false;
        if (g_context) g_retired.push_back({g_context, g_presentCount});
        g_context = nullptr;
        g_key = key;
        g_failed = !InitContext(key);
        if (g_failed) return false;
    }

    // Render-extent UV units -> input pixels.
    const float scaleX = in.mvScaleX * static_cast<float>(in.renderSize.width);
    const float scaleY = in.mvScaleY * static_cast<float>(in.renderSize.height);
    if (scaleX != g_velocityScale[0] || scaleY != g_velocityScale[1]) {
        g_api.setVelocityScale(g_context, scaleX, scaleY);
        g_velocityScale[0] = scaleX;
        g_velocityScale[1] = scaleY;
    }

    xess_d3d12_execute_params_t p{};
    p.pColorTexture = in.color;
    p.pVelocityTexture = in.motionVectors;
    p.pDepthTexture = in.depth;
    p.pOutputTexture = in.output;
    // XeSS refuses a shift beyond half a pixel. The frame the render size changes still has the
    // previous size's jitter (up to 2.3 pixels of the new one, Ultra Performance -> Ultra Quality);
    // its history is reset anyway.
    p.jitterOffsetX = std::clamp(in.jitterX, -0.5f, 0.5f);
    p.jitterOffsetY = std::clamp(in.jitterY, -0.5f, 0.5f);
    p.exposureScale = 1.0f;
    p.resetHistory = in.reset ? 1u : 0u;
    p.inputWidth = in.renderSize.width;
    p.inputHeight = in.renderSize.height;
    if (const xess_result_t r = g_api.execute(g_context, in.list, &p); r < XESS_RESULT_SUCCESS) {
        log::Error("XeSS: execute ({}x{} -> {}x{}) failed: {}", in.renderSize.width, in.renderSize.height,
                   in.outputSize.width, in.outputSize.height, ResultName(r));
        return false;
    }
    return true;
}

void OnPresent() {
    ++g_presentCount;
    std::erase_if(g_retired, [](const Retired& r) {
        if (g_presentCount - r.present < kRetireFrames) return false;
        g_api.destroyContext(r.context);
        return true;
    });
}

}  // namespace dsru::xess

#else  // !DSRU_HAS_XESS

namespace dsru::xess {

void OnDeviceCreated(ID3D12Device*) {}
void Load() { log::Warn("XeSS: built without the XeSS SDK"); }
bool Available() { return false; }
std::string VersionName() { return {}; }
bool Evaluate(const UpscaleInputs&) { return false; }
void OnPresent() {}

}  // namespace dsru::xess

#endif
