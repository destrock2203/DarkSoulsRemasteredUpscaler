#include "upscaler/fsr/fsr.h"

#include "core/log.h"

#ifdef DSRU_HAS_FIDELITYFX

#include "upscaler/fsr/ffx_runtime.h"

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

#include <atomic>
#include <mutex>

namespace dsru::fsr {

namespace {

ID3D12Device* g_device = nullptr;
std::once_flag g_loadOnce;
std::atomic<bool> g_available{false};
std::vector<Version> g_versions;  // written once in Load

std::string g_versionSetting;  // the user's choice, empty = default

// The live context and what it was created for.
struct Key {
    uint32_t width = 0, height = 0;
    bool hdr = false;
    bool invertedDepth = false;
    std::string version;
    bool operator==(const Key&) const = default;
};
ffxContext g_context = nullptr;
Key g_key;
std::string g_activeVersion;
bool g_failed = false;  // context creation failed for g_key: don't retry every frame

// Contexts replaced while command lists recorded with them may still be in flight.
struct Retired {
    ffxContext context;
    uint64_t present;
};
std::vector<Retired> g_retired;
uint64_t g_presentCount = 0;
constexpr uint64_t kRetireFrames = 8;

void DoLoad() {
    if (!g_device) {
        log::Warn("FSR: no D3D12 device yet");
        return;
    }
    if (!ffx::Load()) return;
    const ffx::Functions& api = ffx::Api();

    // Upscaler versions for this device (providers the driver doesn't support aren't listed).
    uint64_t count = 0;
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = g_device;
    query.outputCount = &count;
    if (const ffxReturnCode_t r = api.query(nullptr, &query.header); r != FFX_API_RETURN_OK || !count) {
        log::Warn("FSR: no upscaler for this device ({})", ffx::ReturnCode(r));
        return;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    query.versionIds = ids.data();
    query.versionNames = names.data();
    if (api.query(nullptr, &query.header) != FFX_API_RETURN_OK) return;
    for (uint64_t i = 0; i < count; ++i) {
        g_versions.push_back({ids[i], names[i] ? names[i] : "?"});
        log::Info("FSR: upscaler version {} available (id 0x{:x})", g_versions.back().name, ids[i]);
    }
    g_available = true;
}

void Retire() {
    if (g_context) g_retired.push_back({g_context, g_presentCount});
    g_context = nullptr;
    g_activeVersion.clear();
}

bool CreateContext(const Key& key) {
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g_device;

    ffxOverrideVersion versionOverride{};
    versionOverride.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    for (const Version& v : g_versions)
        if (v.name == key.version) versionOverride.versionId = v.id;
    if (versionOverride.versionId) backend.header.pNext = &versionOverride.header;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.flags = FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (key.invertedDepth) desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    // The game's HDR scene (native AA) gets auto exposure; the tonemapped scene (upscaling) is sRGB.
    desc.flags |= key.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
                          : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = {key.width, key.height};
    desc.maxUpscaleSize = {key.width, key.height};
    desc.fpMessage = ffx::Message;

    ffxContext context = nullptr;
    if (const ffxReturnCode_t r = ffx::Api().createContext(&context, &desc.header, nullptr); r != FFX_API_RETURN_OK) {
        log::Error("FSR: context creation for {}x{} failed: {}", key.width, key.height, ffx::ReturnCode(r));
        return false;
    }
    g_context = context;
    g_key = key;

    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx::Api().query(&g_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
        g_activeVersion = provider.versionName;
    log::Info("FSR {}: context for {}x{} ({})", g_activeVersion.empty() ? "?" : g_activeVersion, key.width,
              key.height, key.hdr ? "HDR" : "sRGB");
    return true;
}

FfxApiResource Resource(ID3D12Resource* resource, uint32_t state) { return ffxApiGetResourceDX12(resource, state); }

}  // namespace

void OnDeviceCreated(ID3D12Device* device) {
    if (!g_device) g_device = device;
}

void Load() { std::call_once(g_loadOnce, DoLoad); }

bool Available() { return g_available.load(); }

const std::vector<Version>& Versions() {
    static const std::vector<Version> none;
    return g_available ? g_versions : none;
}

void SetVersion(const std::string& name) {
    if (name == g_versionSetting) return;
    g_versionSetting = name;
    g_failed = false;
}

std::string ActiveVersion() { return g_activeVersion; }

bool Evaluate(const UpscaleInputs& in) {
    if (!g_available) return false;
    const Key key{in.outputSize.width, in.outputSize.height, in.colorHdr, in.depthInverted, g_versionSetting};
    if (!g_context || !(key == g_key)) {
        if (g_failed && key == g_key) return false;
        Retire();
        g_key = key;
        g_failed = !CreateContext(key);
        if (g_failed) return false;
    }

    constexpr uint32_t kRead = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = in.list;
    d.color = Resource(in.color, kRead);
    d.depth = Resource(in.depth, kRead);
    d.motionVectors = Resource(in.motionVectors, kRead);
    d.output = Resource(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {in.jitterX, in.jitterY};
    // Render-extent UV units -> render pixels.
    d.motionVectorScale = {in.mvScaleX * static_cast<float>(in.renderSize.width),
                           in.mvScaleY * static_cast<float>(in.renderSize.height)};
    d.renderSize = {in.renderSize.width, in.renderSize.height};
    d.upscaleSize = {in.outputSize.width, in.outputSize.height};
    d.enableSharpening = false;
    d.frameTimeDelta = in.frameTimeMs;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    // FidelityFX takes the distances at depth 0 and depth 1: swapped for inverted depth (AMD's samples
    // pass cameraNear = FLT_MAX, cameraFar = the near plane).
    d.cameraNear = in.depthInverted ? in.cameraFar : in.cameraNear;
    d.cameraFar = in.depthInverted ? in.cameraNear : in.cameraFar;
    d.cameraFovAngleVertical = in.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = in.colorHdr ? 0u : static_cast<uint32_t>(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB);
    if (const ffxReturnCode_t r = ffx::Api().dispatch(&g_context, &d.header); r != FFX_API_RETURN_OK) {
        log::Error("FSR: dispatch failed: {}", ffx::ReturnCode(r));
        return false;
    }
    return true;
}

void OnPresent() {
    ++g_presentCount;
    std::erase_if(g_retired, [](const Retired& r) {
        if (g_presentCount - r.present < kRetireFrames) return false;
        ffxContext context = r.context;
        ffx::Api().destroyContext(&context, nullptr);
        return true;
    });
}

}  // namespace dsru::fsr

#else  // !DSRU_HAS_FIDELITYFX

namespace dsru::fsr {

void OnDeviceCreated(ID3D12Device*) {}
void Load() { log::Warn("FSR: built without the FidelityFX SDK"); }
bool Available() { return false; }
const std::vector<Version>& Versions() {
    static const std::vector<Version> none;
    return none;
}
void SetVersion(const std::string&) {}
std::string ActiveVersion() { return {}; }
bool Evaluate(const UpscaleInputs&) { return false; }
void OnPresent() {}

}  // namespace dsru::fsr

#endif
