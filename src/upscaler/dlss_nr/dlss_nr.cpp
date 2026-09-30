#include "upscaler/dlss_nr/dlss_nr.h"

#include "core/log.h"
#include "core/paths.h"
#include "upscaler/signature.h"

#ifdef DSRU_HAS_NGX

#include <nvsdk_ngx.h>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dsru::upscaler::dlss_nr {

namespace {

// Application id NGX expects from titles without an NVIDIA-assigned one.
constexpr unsigned long long kAppId = 0x1000000ull;
constexpr NVSDK_NGX_Feature kFeature = NVSDK_NGX_Feature_Reserved18;  // DLSS 5 neural rendering
// Evaluations before a replaced feature is released (it may still be in flight on the GPU).
constexpr uint64_t kRetireDelay = 8;

using InitExt_t = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                       const NVSDK_NGX_Parameter*);
using Create_t = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*,
                                      NVSDK_NGX_Handle**);
using Evaluate_t = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                        const NVSDK_NGX_Parameter*);
using Release_t = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);
using Bind_t = int (*)(HMODULE);

enum class State { Untried, Ready, Failed };
State g_state = State::Untried;
const char* g_reason = "";

InitExt_t g_initExt = nullptr;
Create_t g_create = nullptr;
Evaluate_t g_evaluate = nullptr;
Release_t g_release = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;

NVSDK_NGX_Handle* g_feature = nullptr;
uint32_t g_featureWidth = 0, g_featureHeight = 0;
Options g_featureOptions;         // tuning the feature was created with (the model latches it)
uint64_t g_optionsChangedAt = 0;  // evaluation when the requested tuning first differed from it
// Evaluations a tuning change must hold before the feature is rebuilt: rebuilding too often
// exhausts the driver's latches and the feature stops responding until the process restarts.
constexpr uint64_t kRebuildSettle = 30;
struct Retired {
    NVSDK_NGX_Handle* handle;
    uint64_t releaseAt;
};
std::vector<Retired> g_retired;
uint64_t g_evaluations = 0;

bool Fail(const char* reason) {
    g_state = State::Failed;
    g_reason = reason;
    return false;
}

// NGX results in the log: 0xBAD000xx codes.
unsigned Code(NVSDK_NGX_Result r) { return static_cast<unsigned>(r); }

// g_params is the driver core's capability block: it carries the snippet and preset callbacks the
// model polls at create time (a block from AllocateParameters has none: "PollRuntimeParams -
// callback is NULL"). Its vtable is not the header's: uints are at slot 3, resources are written as
// 64-bit values at slot 0 (the typed D3D12 setter leaves them unset), and the float setter's slot is
// found by a round trip (ProbeFloatSlot). The layout OptiScaler_DLSSNR drives.
constexpr int kSlotULL = 0;
constexpr int kSlotUInt = 3;
int g_floatSlot = -1;

using SetULL_t = void (*)(NVSDK_NGX_Parameter*, const char*, unsigned long long);
using SetFloat_t = void (*)(NVSDK_NGX_Parameter*, const char*, float);
using SetUInt_t = void (*)(NVSDK_NGX_Parameter*, const char*, unsigned int);

void** Vtable(NVSDK_NGX_Parameter* p) { return *reinterpret_cast<void***>(p); }

void SetU(const char* name, uint32_t v) { reinterpret_cast<SetUInt_t>(Vtable(g_params)[kSlotUInt])(g_params, name, v); }

void SetF(const char* name, float v) {
    if (g_floatSlot >= 0) reinterpret_cast<SetFloat_t>(Vtable(g_params)[g_floatSlot])(g_params, name, v);
}

void SetR(const char* name, ID3D12Resource* v) {
    reinterpret_cast<SetULL_t>(Vtable(g_params)[kSlotULL])(g_params, name, reinterpret_cast<unsigned long long>(v));
}

// Writes a known float through each candidate slot and keeps the first that reads back exactly.
void ProbeFloatSlot() {
    constexpr const char* kKey = "DLSSNR.DsruFloatProbe";
    constexpr float kExpected = 0.375f;  // exact in binary
    for (const int slot : {1, 2, 5, 6, 7, 4, 3, 0}) {
        reinterpret_cast<SetFloat_t>(Vtable(g_params)[slot])(g_params, kKey, kExpected);
        float readBack = 0.0f;
        if (g_params->Get(kKey, &readBack) == NVSDK_NGX_Result_Success && readBack == kExpected) {
            g_floatSlot = slot;
            log::Info("DLSS 5: float parameters go through vtable slot {}", slot);
            return;
        }
    }
    log::Warn("DLSS 5: float setter not found - intensity / local tone / structure will have no effect");
}

// The tuning. The model reads it once, when it builds the feature.
void SetTuning(const Options& o) {
    SetF("DLSSNR.Intensity", o.intensity);
    SetU("DLSSNR.Style", o.style);
    SetF("DLSSNR.LocalStructureStrength", o.localStructure);
    SetF("DLSSNR.LocalToneStrength", o.localTone);
    SetF("DLSSNR.SkinStructureStrength", o.skinStructure);
    // The input is the HUD-less scene: no UI to protect.
    SetU("DLSSNR.UseAutoMask", 0u);
    SetU("DLSSNR.UICorrection", 0u);
}

bool SameTuning(const Options& a, const Options& b) {
    return a.intensity == b.intensity && a.localTone == b.localTone && a.localStructure == b.localStructure &&
           a.skinStructure == b.skinStructure && a.style == b.style;
}

// NGX's own log lines (why a call failed), forwarded into ours.
void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature component) {
    std::string_view line = message ? message : "";
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (!line.empty()) log::Info("[NGX {}] {}", static_cast<int>(component), line);
}

// Compute capability of the CUDA device behind the D3D12 adapter, as 10 * major + minor (120 for
// RTX 50, 89 for RTX 40, 86 for RTX 30), or 0 if unknown. nvcuda.dll ships with every driver.
int GpuArch(ID3D12Device* device) {
    using cuInit_t = int(__stdcall*)(unsigned);
    using cuDeviceGetCount_t = int(__stdcall*)(int*);
    using cuDeviceGet_t = int(__stdcall*)(int*, int);
    using cuDeviceGetAttribute_t = int(__stdcall*)(int*, int, int);
    using cuDeviceGetLuid_t = int(__stdcall*)(char*, unsigned*, int);
    constexpr int kMajor = 75, kMinor = 76;  // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR / MINOR
    HMODULE cuda = LoadLibraryW(L"nvcuda.dll");
    if (!cuda) return 0;
    const auto init = reinterpret_cast<cuInit_t>(GetProcAddress(cuda, "cuInit"));
    const auto count = reinterpret_cast<cuDeviceGetCount_t>(GetProcAddress(cuda, "cuDeviceGetCount"));
    const auto get = reinterpret_cast<cuDeviceGet_t>(GetProcAddress(cuda, "cuDeviceGet"));
    const auto attribute = reinterpret_cast<cuDeviceGetAttribute_t>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
    const auto luid = reinterpret_cast<cuDeviceGetLuid_t>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    int devices = 0;
    if (!init || !count || !get || !attribute || init(0) != 0 || count(&devices) != 0) return 0;
    const LUID adapter = device->GetAdapterLuid();
    int arch = 0;
    for (int i = 0; i < devices; ++i) {
        int dev = 0, major = 0, minor = 0;
        if (get(&dev, i) != 0 || attribute(&major, kMajor, dev) != 0 || attribute(&minor, kMinor, dev) != 0) continue;
        char id[8] = {};
        unsigned mask = 0;
        const bool same = luid && luid(id, &mask, dev) == 0 && std::memcmp(id, &adapter, sizeof(id)) == 0;
        if (same || !arch) arch = 10 * major + minor;
        if (same) break;
    }
    return arch;
}

// Architectures of the CUDA kernels (cubins) embedded in a runtime, as 10 * major + minor. Empty
// when none are found in plain form (compressed or PTX-only builds).
std::set<int> KernelArchs(const std::filesystem::path& file) {
    std::set<int> archs;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return archs;
    HANDLE mapping = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const auto* data = mapping ? static_cast<const unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    if (data) {
        constexpr unsigned char kElf[] = {0x7F, 'E', 'L', 'F', 2, 1, 1};  // 64-bit, little endian
        const auto* end = data + size.QuadPart;
        for (const auto* p = data; end - p > 0x40;) {
            p = std::search(p, end - 0x40, std::begin(kElf), std::end(kElf));
            if (p >= end - 0x40) break;
            if (p[7] == 0x33 || p[7] == 0x41) {  // CUDA ELF ABI
                uint32_t flags;
                std::memcpy(&flags, p + 0x30, sizeof(flags));
                const unsigned sm = (flags >> 8) & 0xFF ? (flags >> 8) & 0xFF : flags & 0xFF;
                if (sm >= 30) archs.insert(static_cast<int>(sm));
            }
            p += sizeof(kElf);
        }
        UnmapViewOfFile(data);
    }
    if (mapping) CloseHandle(mapping);
    CloseHandle(f);
    return archs;
}

// Machine code built for `kernel` runs on `gpu` within the same major version, minor >= its own.
bool Runs(int kernel, int gpu) { return kernel / 10 == gpu / 10 && kernel % 10 <= gpu % 10; }

// Picks the runtime for this GPU among DARK SOULS REMASTERED/DSRUpscaler/nvngx_dlssnr*.dll (the stock build
// has RTX 50 kernels only; repacks carry RTX 40 or older ones). Prefers kernels that run on this
// GPU, then builds whose kernels can't be read (may carry a generic path), then NVIDIA's signature,
// then the plain name.
std::filesystem::path PickRuntime(const std::filesystem::path& dir, ID3D12Device* device) {
    const int gpu = GpuArch(device);
    log::Info("DLSS 5: GPU architecture sm_{}", gpu ? std::to_string(gpu) : std::string("unknown"));
    std::filesystem::path best;
    int bestScore = -1;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::filesystem::path path = entry.path();
        std::wstring name = path.filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        if (name.rfind(L"nvngx_dlssnr", 0) != 0 || path.extension() != L".dll") continue;
        const std::set<int> archs = KernelArchs(path);
        const bool runs = gpu && std::any_of(archs.begin(), archs.end(), [&](int a) { return Runs(a, gpu); });
        const bool signedByNvidia = VerifySignature(path, L"NVIDIA");
        std::string list;
        for (const int a : archs) list += (list.empty() ? "sm_" : ", sm_") + std::to_string(a);
        const int fit = runs ? 2 : (archs.empty() || !gpu) ? 1 : 0;
        log::Info("DLSS 5: candidate '{}': kernels [{}], {} - {}", path.filename().string(),
                  list.empty() ? "unreadable" : list, signedByNvidia ? "NVIDIA signed" : "modified/unsigned",
                  fit == 2 ? "runs on this GPU" : fit == 1 ? "unknown fit" : "does NOT run on this GPU");
        if (!fit) continue;
        const int score = fit * 4 + (signedByNvidia ? 2 : 0) + (name == L"nvngx_dlssnr.dll" ? 1 : 0);
        if (score > bestScore) {
            bestScore = score;
            best = path;
        }
    }
    return best;
}

bool Load(ID3D12Device* device) {
    const std::filesystem::path dir = paths::ModuleDirectory() / "DSRUpscaler";
    const std::filesystem::path runtime = PickRuntime(dir, device);
    if (runtime.empty()) {
        log::Warn("DLSS 5: no nvngx_dlssnr*.dll for this GPU in '{}' (RTX 50 needs NVIDIA's own build)",
                  dir.string());
        return Fail("no nvngx_dlssnr.dll for this GPU in DARK SOULS REMASTERED/DSRUpscaler");
    }
    log::Info("DLSS 5: using '{}'", runtime.filename().string());
    // Debug logging: the runtime's own verbose log (nvngx_dlssnr*.log next to it), read at load time.
    const bool verbose = log::Enabled(log::Level::Debug);
    if (verbose) {
        SetEnvironmentVariableW(L"__NGX_ENABLE_OVERRIDE_LOG_PATH", L"1");
        SetEnvironmentVariableW(L"__NGX_LOG_PATH_OVERRIDE", dir.wstring().c_str());
        SetEnvironmentVariableW(L"__NGX_LOG_LEVEL", L"2");
    }
    HMODULE library = LoadLibraryW(runtime.c_str());
    if (!library) {
        log::Error("DLSS 5: failed to load '{}' (error {})", runtime.string(), GetLastError());
        return Fail("nvngx_dlssnr.dll failed to load");
    }
    HMODULE forwarder = LoadLibraryW((dir / "dsru_nvngx.dll").c_str());
    const auto bind = forwarder ? reinterpret_cast<Bind_t>(GetProcAddress(forwarder, "DsruNrBind")) : nullptr;
    if (!bind) {
        log::Error("DLSS 5: dsru_nvngx.dll missing from '{}'", dir.string());
        return Fail("dsru_nvngx.dll missing");
    }
    if (bind(library) != 0) {
        log::Error("DLSS 5: nvngx_dlssnr.dll lacks the NGX entry points - unsupported version");
        return Fail("unsupported nvngx_dlssnr.dll");
    }
    g_initExt = reinterpret_cast<InitExt_t>(GetProcAddress(forwarder, "DsruNrInitExt"));
    g_create = reinterpret_cast<Create_t>(GetProcAddress(forwarder, "DsruNrCreate"));
    g_evaluate = reinterpret_cast<Evaluate_t>(GetProcAddress(forwarder, "DsruNrEvaluate"));
    g_release = reinterpret_cast<Release_t>(GetProcAddress(forwarder, "DsruNrRelease"));
    if (!g_initExt || !g_create || !g_evaluate || !g_release) return Fail("dsru_nvngx.dll is outdated");

    // NGX itself (driver side), for the parameter block, then the feature runtime.
    const std::wstring dataPath = dir.wstring();
    NVSDK_NGX_FeatureCommonInfo info{};
    info.LoggingInfo.LoggingCallback = NgxLog;
    // NGX's lines go to our log only (no nvngx*.log files), verbose in debug logging.
    info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = !verbose;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(kAppId, dataPath.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(r)) {
        log::Error("DLSS 5: NGX initialization failed (0x{:08X})", Code(r));
        return Fail("NGX initialization failed (driver)");
    }
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) {
        g_params = nullptr;
        log::Error("DLSS 5: NGX capability parameters unavailable (0x{:08X})", Code(r));
        return Fail("NGX capability parameters unavailable (driver)");
    }
    ProbeFloatSlot();
    r = g_initExt(kAppId, dataPath.c_str(), device, NVSDK_NGX_Version_API, g_params);
    if (NVSDK_NGX_FAILED(r)) {
        log::Error("DLSS 5: runtime initialization failed (0x{:08X})", Code(r));
        return Fail("nvngx_dlssnr.dll refused to initialize (see log)");
    }
    log::Info("DLSS 5: neural rendering runtime loaded from '{}'", runtime.string());
    g_state = State::Ready;
    return true;
}

void RetireFeature() {
    if (g_feature) g_retired.push_back({g_feature, g_evaluations + kRetireDelay});
    g_feature = nullptr;
}

void ReleaseRetired(bool all) {
    for (auto it = g_retired.begin(); it != g_retired.end();) {
        if (all || it->releaseAt <= g_evaluations) {
            g_release(it->handle);
            it = g_retired.erase(it);
        } else {
            ++it;
        }
    }
}

bool EnsureFeature(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, const Options& options) {
    if (g_feature && width == g_featureWidth && height == g_featureHeight) {
        if (SameTuning(options, g_featureOptions)) {
            g_optionsChangedAt = 0;
            return true;
        }
        if (!g_optionsChangedAt) g_optionsChangedAt = g_evaluations;
        if (g_evaluations - g_optionsChangedAt < kRebuildSettle) return true;
    }
    RetireFeature();
    g_optionsChangedAt = 0;
    // No Reset(): the block is the driver's and holds its callbacks. Every value is written instead.
    SetU("DLSSNR.Enabled", 1u);
    SetU("DLSSNR.Width", width);
    SetU("DLSSNR.Height", height);
    SetU(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    SetU(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    SetU("DLSSNR.Hint.Render.Preset", 0u);
    SetTuning(options);
    const NVSDK_NGX_Result r = g_create(list, kFeature, g_params, &g_feature);
    if (NVSDK_NGX_FAILED(r) || !g_feature) {
        g_feature = nullptr;
        log::Error("DLSS 5: feature creation failed at {}x{} (0x{:08X}) - reason in DSRUpscaler/nvngx_dlssnr_*.log",
                   width, height, Code(r));
        return Fail("feature creation failed (see log)");
    }
    g_featureWidth = width;
    g_featureHeight = height;
    g_featureOptions = options;
    log::Info("DLSS 5: feature created ({}x{}, intensity {}, style {}, local tone {}, local structure {}, skin {})",
              width, height, options.intensity, options.style, options.localTone, options.localStructure,
              options.skinStructure);
    return true;
}

// No C++ objects here: __try can't be mixed with unwinding.
NVSDK_NGX_Result GuardedEvaluate(ID3D12GraphicsCommandList* list, DWORD& exception) {
    exception = 0;
    __try {
        return g_evaluate(list, g_feature, g_params);
    } __except (exception = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return NVSDK_NGX_Result_Fail;
    }
}

}  // namespace

bool Evaluate(const Inputs& in, const Options& options) {
    if (g_state == State::Failed) return false;
    if (!in.list || !in.color || !in.depth || !in.motionVectors || !in.output || !in.width || !in.height) return false;
    if (g_state == State::Untried && !Load(in.device)) return false;
    ++g_evaluations;
    if (!EnsureFeature(in.list, in.width, in.height, options)) return false;
    ReleaseRetired(false);

    // The block is shared with every NGX session in the process: everything is written every frame.
    SetR("DLSSNR.Color", in.color);
    SetR("DLSSNR.Depth", in.depth);
    SetR("DLSSNR.MVec", in.motionVectors);
    SetR("DLSSNR.Output", in.output);
    SetU("DLSSNR.Enabled", 1u);
    SetU("DLSSNR.Width", in.width);
    SetU("DLSSNR.Height", in.height);
    SetU("DLSSNR.DepthInverted", in.depthInverted ? 1u : 0u);
    SetU("DLSSNR.Reset", in.reset ? 1u : 0u);
    SetU("DLSSNR.ColorSubrectBaseX", 0u);
    SetU("DLSSNR.ColorSubrectBaseY", 0u);
    SetU("DLSSNR.ColorSubrectWidth", in.width);
    SetU("DLSSNR.ColorSubrectHeight", in.height);
    SetU("DLSSNR.OutputSubrectBaseX", 0u);
    SetU("DLSSNR.OutputSubrectBaseY", 0u);
    SetU("DLSSNR.OutputSubrectWidth", in.width);
    SetU("DLSSNR.OutputSubrectHeight", in.height);
    SetU("DLSSNR.DepthSubrectBaseX", 0u);
    SetU("DLSSNR.DepthSubrectBaseY", 0u);
    SetU("DLSSNR.DepthSubrectWidth", in.mvWidth);
    SetU("DLSSNR.DepthSubrectHeight", in.mvHeight);
    SetU("DLSSNR.MVecSubrectBaseX", 0u);
    SetU("DLSSNR.MVecSubrectBaseY", 0u);
    SetU("DLSSNR.MVecSubrectWidth", in.mvWidth);
    SetU("DLSSNR.MVecSubrectHeight", in.mvHeight);
    SetF("DLSSNR.MVecScaleX", in.mvScaleX);
    SetF("DLSSNR.MVecScaleY", in.mvScaleY);
    SetTuning(g_featureOptions);

    DWORD exception = 0;
    const NVSDK_NGX_Result r = GuardedEvaluate(in.list, exception);
    if (exception) {
        log::Error("DLSS 5: evaluation raised exception 0x{:08X} - disabled", static_cast<unsigned>(exception));
        return Fail("crashed during evaluation (see log)");
    }
    if (NVSDK_NGX_FAILED(r)) {
        log::Error("DLSS 5: evaluation failed (0x{:08X}) - disabled", Code(r));
        return Fail("evaluation failed (see log)");
    }
    return true;
}

const char* UnavailableReason() { return g_reason; }

void Shutdown(ID3D12Device* device) {
    if (g_state != State::Ready) return;
    RetireFeature();
    ReleaseRetired(true);
    if (g_params) NVSDK_NGX_D3D12_DestroyParameters(g_params);
    g_params = nullptr;
    if (device) NVSDK_NGX_D3D12_Shutdown1(device);
    g_state = State::Failed;
    g_reason = "shut down";
}

}  // namespace dsru::upscaler::dlss_nr

#else  // !DSRU_HAS_NGX

namespace dsru::upscaler::dlss_nr {

bool Evaluate(const Inputs&, const Options&) { return false; }
const char* UnavailableReason() { return "built without the NGX SDK (third_party/ngx)"; }
void Shutdown(ID3D12Device*) {}

}  // namespace dsru::upscaler::dlss_nr

#endif
