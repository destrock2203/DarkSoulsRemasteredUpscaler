#include "render/upscale.h"

#include "core/config.h"
#include "core/log.h"
#include "framegen/framegen.h"
#include "hooks/d3d11_hooks.h"
#include "render/bridge.h"
#include "render/d3d11_util.h"
#include "render/presenter.h"
#include "render/sharpen.h"
#include "upscaler/dlss_nr/dlss_nr.h"
#include "upscaler/fsr/fsr.h"
#include "upscaler/streamline/streamline.h"
#include "upscaler/upscaler.h"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace dsru::render::upscale {

namespace {

using hooks::d3d11::ContextState;

// Resolve constants (FRPG_Fil_ResolveTAA.fpo, cb0 registers).
constexpr uint32_t kRegDepthParams = 8;   // y, z, w: linear depth = w / (z * depth + y)
constexpr uint32_t kRegScreenSize = 12;   // width, height, 1 / width, 1 / height
constexpr uint32_t kRegClipToWorld = 58;  // 58..61
constexpr uint32_t kRegWorldToPrevClip = 74;  // 74..77
constexpr uint32_t kRegCameraOffset = 78;
constexpr size_t kTaaRegisters = 79;

// Everything below (TAA / velocity draws come from the engine's worker threads). Recursive: on the
// immediate context AtExecute runs its callback right away, inside OnTaaDraw / OnHudDraw.
std::recursive_mutex g_mutex;

// Settings.
bool g_enabled = true;
Backend g_backend = Backend::Dlss;
uint32_t g_dlssPreset = 0;
std::string g_fsrVersion;
bool g_flipX = false, g_flipY = false;

// Shared with D3D12.
bridge::SharedTexture g_color, g_depth, g_motion, g_output, g_hudless;
// The scene right before the HUD (the game draws the HUD into the tonemapped scene target, then its
// final pass applies the brightness setting into the back buffer). D3D11 only: it goes through that
// final pass into g_hudless (OnGammaDraw).
ComPtr<ID3D11Texture2D> g_scenePre;
ComPtr<ID3D11ShaderResourceView> g_scenePreSrv;
bool g_scenePreReady = false;  // set when the frame's command list with the copy has executed
// Upscaling (the game renders below the output size): the tonemapped scene at the render size, and the
// upscaler's image at the output size, over which the HUD is then drawn.
bridge::SharedTexture g_sceneLdr, g_upscaled;
// Set at the TAA resolve of an upscaled frame, used at its first HUD draw (same command list).
bool g_pendingValid = false;
UpscaleInputs g_pending;
bool g_pendingScaleOnly = false;  // upscaler off: the game's TAA image only scaled to the output, for DLSS 5
bool g_upscaledReady = false;  // the frame's upscaled scene + HUD exist (its command list executed)
uint32_t g_outputWidth = 0, g_outputHeight = 0;
QualityMode g_activeMode = QualityMode::Native;

// DLSS 5 neural rendering (upscaler/dlss_nr) on the HUD-less scene: after the upscaler on its output,
// or at native resolution on the scene copied at the first HUD draw (g_nrScene).
bool g_dlss5Enabled = false;
bool g_dlss5Reset = true;
upscaler::dlss_nr::Options g_dlss5;
std::atomic<uint64_t> g_dlss5RanAt{0};
bool g_dlss5KeyDown = false;
Microsoft::WRL::ComPtr<ID3D12Resource> g_nrOutput;  // D3D12 only, UNORDERED_ACCESS
// Sharpening of the display-referred scene (render/sharpen.h, streamline::EvaluateNis), after DLSS 5.
SharpenFilter g_sharpenFilter = SharpenFilter::Off;
float g_sharpness = 0.5f;
Microsoft::WRL::ComPtr<ID3D12Resource> g_sharpenOutput;  // D3D12 only, UNORDERED_ACCESS
bridge::SharedTexture g_nrScene;                    // native resolution: the scene, RGBA16F
bool g_nativePendingValid = false;                  // TAA inputs of the frame, for DLSS 5 at its HUD
UpscaleInputs g_nativePending;

// From the velocity pass (latest).
struct Camera {
    bool valid = false;
    float jitterNdc[2]{};
    float viewProj[4][4]{};      // this frame, unjittered (rows, clip = M * p)
    float prevViewProj[4][4]{};  // previous frame
};
Camera g_camera;

// Per frame, set when the frame's command lists execute (AtExecute), taken at Present.
struct FrameState {
    bool hasInputs = false;
    bool hasHudless = false;
    UpscaleInputs inputs;
};
FrameState g_frame;

// Status.
std::atomic<uint64_t> g_present{0};
std::atomic<uint64_t> g_taaSeenAt{0}, g_velocitySeenAt{0}, g_ranAt{0};
std::atomic<bool> g_failed{false};
std::atomic<int> g_failures{0};
std::atomic<bool> g_split{false};
std::atomic<bool> g_lastRunOk{false};  // the latest RunUpscaler call succeeded
const char* g_reason = "";
uint32_t g_width = 0, g_height = 0;
float g_near = 0, g_far = 0, g_fovY = 0;
float g_lastJitter[2]{};
bool g_resetNext = true;
LARGE_INTEGER g_lastTaaTime{};
bool g_keyDown = false;

void Read4(const std::vector<uint8_t>& data, uint32_t reg, float out[4]) {
    std::memcpy(out, data.data() + reg * 16, 16);
}

float Length3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Projection / view of a view-projection (rows, clip = M * p) with a rigid view: the rows of M are
// xs * right, ys * up, (A, B terms) and forward (P's last row is (0, 0, 1, 0)).
void Decompose(const float vp[4][4], float nearPlane, float farPlane, float projection[16], float view[16], float& fovY,
               float& aspect) {
    const float xs = Length3(vp[0]), ys = Length3(vp[1]);
    fovY = ys > 0 ? 2.0f * std::atan(1.0f / ys) : 1.0f;
    aspect = xs > 0 ? ys / xs : 1.0f;
    std::memset(projection, 0, sizeof(float) * 16);
    projection[0] = xs;
    projection[5] = ys;
    const float range = farPlane - nearPlane;
    projection[10] = range != 0 ? farPlane / range : 1.0f;
    projection[11] = range != 0 ? -nearPlane * farPlane / range : 0.0f;
    projection[14] = 1.0f;
    for (int c = 0; c < 4; ++c) {
        view[0 * 4 + c] = xs > 0 ? vp[0][c] / xs : 0.0f;
        view[1 * 4 + c] = ys > 0 ? vp[1][c] / ys : 0.0f;
        view[2 * 4 + c] = vp[3][c];
        view[3 * 4 + c] = c == 3 ? 1.0f : 0.0f;
    }
}

bool UpscalerWanted() { return g_enabled && !g_failed; }

// DLSS 5 on `color` (RGBA16F, COMMON, color size = its whole extent), in place: into g_nrOutput, then
// copied back. Depth / motion vectors at the render size. Immediate context thread, g_mutex not held.
bool NeuralRender(ID3D12GraphicsCommandList* list, ID3D12Resource* color, const UpscaleInputs& in, bool reset,
                  const upscaler::dlss_nr::Options& options) {
    const D3D12_RESOURCE_DESC desc = color->GetDesc();
    const auto width = static_cast<uint32_t>(desc.Width);
    const uint32_t height = desc.Height;
    if (!g_nrOutput || g_nrOutput->GetDesc().Width != desc.Width || g_nrOutput->GetDesc().Height != desc.Height) {
        g_nrOutput.Reset();
        D3D12_RESOURCE_DESC out = desc;
        out.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        if (FAILED(bridge::Device12()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &out,
                                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                               IID_PPV_ARGS(&g_nrOutput))))
            return false;
        g_nrOutput->SetName(L"DSRUpscaler DLSS 5 output");
    }
    constexpr D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER before[3] = {bridge::Transition(color, D3D12_RESOURCE_STATE_COMMON, kRead),
                                        bridge::Transition(in.depth, D3D12_RESOURCE_STATE_COMMON, kRead),
                                        bridge::Transition(in.motionVectors, D3D12_RESOURCE_STATE_COMMON, kRead)};
    list->ResourceBarrier(3, before);
    upscaler::dlss_nr::Inputs nr;
    nr.device = bridge::Device12();
    nr.list = list;
    nr.color = color;
    nr.width = width;
    nr.height = height;
    nr.depth = in.depth;
    nr.depthInverted = false;
    nr.motionVectors = in.motionVectors;
    nr.mvWidth = in.renderSize.width;
    nr.mvHeight = in.renderSize.height;
    // prevUV - curUV in UV units -> pixels of the color image (current -> previous).
    nr.mvScaleX = static_cast<float>(width);
    nr.mvScaleY = static_cast<float>(height);
    nr.output = g_nrOutput.Get();
    nr.reset = reset;
    const bool ok = upscaler::dlss_nr::Evaluate(nr, options);
    D3D12_RESOURCE_BARRIER after[4] = {bridge::Transition(color, kRead, D3D12_RESOURCE_STATE_COPY_DEST),
                                       bridge::Transition(in.depth, kRead, D3D12_RESOURCE_STATE_COMMON),
                                       bridge::Transition(in.motionVectors, kRead, D3D12_RESOURCE_STATE_COMMON),
                                       bridge::Transition(g_nrOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                          D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(4, after);
    if (ok) list->CopyResource(color, g_nrOutput.Get());
    D3D12_RESOURCE_BARRIER back[2] = {bridge::Transition(color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
                                      bridge::Transition(g_nrOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, back);
    return ok;
}

struct Dlss5Request {
    bool enabled = false;
    bool reset = false;
    upscaler::dlss_nr::Options options;
};

Dlss5Request Dlss5Now() {
    Dlss5Request r;
    r.enabled = g_dlss5Enabled && upscaler::dlss_nr::UnavailableReason()[0] == '\0';
    r.reset = g_dlss5Reset || g_present.load() - g_dlss5RanAt.load() > 2;
    r.options = g_dlss5;
    return r;
}

void Dlss5Ran(const Dlss5Request& r, bool ok) {
    if (!ok) return;
    g_dlss5RanAt = g_present.load();
    if (r.reset) {
        std::lock_guard lock(g_mutex);
        g_dlss5Reset = false;
    }
}

struct SharpenRequest {
    SharpenFilter filter = SharpenFilter::Off;
    float sharpness = 0.0f;
    bool Wanted() const { return filter != SharpenFilter::Off && sharpness > 0.0f; }
};

SharpenRequest SharpenNow() { return {g_sharpenFilter, g_sharpness}; }
bool SharpenWanted() { return SharpenNow().Wanted(); }

// `color` (RGBA16F, COMMON, display-referred) sharpened in place: the filter into g_sharpenOutput, then
// copied back. NVIDIA Image Scaling (and the Laplacian, if its pass fails) fall back to AMD RCAS.
// Immediate context thread.
void Sharpen(ID3D12GraphicsCommandList* list, ID3D12Resource* color, const SharpenRequest& sr) {
    if (!sr.Wanted()) return;
    const D3D12_RESOURCE_DESC desc = color->GetDesc();
    const auto width = static_cast<uint32_t>(desc.Width);
    const uint32_t height = desc.Height;
    if (!g_sharpenOutput || g_sharpenOutput->GetDesc().Width != desc.Width || g_sharpenOutput->GetDesc().Height != height) {
        g_sharpenOutput.Reset();
        D3D12_RESOURCE_DESC out = desc;
        out.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        if (FAILED(bridge::Device12()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &out,
                                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                               IID_PPV_ARGS(&g_sharpenOutput))))
            return;
        g_sharpenOutput->SetName(L"DSRUpscaler sharpen output");
    }
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_BARRIER before = bridge::Transition(color, D3D12_RESOURCE_STATE_COMMON, kRead);
    list->ResourceBarrier(1, &before);
    bool ok = false;
    if (sr.filter == SharpenFilter::Nvidia) {
        ok = streamline::NisAvailable() &&
             streamline::EvaluateNis(list, color, kRead, g_sharpenOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                     width, height, sr.sharpness);
        static bool logged = false;
        if (!ok && !logged) {
            logged = true;
            log::Warn("Sharpen: NVIDIA Image Scaling unavailable - AMD RCAS instead");
        }
    }
    if (!ok && sr.filter == SharpenFilter::Laplacian)
        ok = sharpen::Laplacian(bridge::Device12(), list, color, g_sharpenOutput.Get(), width, height, sr.sharpness);
    if (!ok)
        ok = sharpen::Rcas(bridge::Device12(), list, color, g_sharpenOutput.Get(), width, height, sr.sharpness);
    const D3D12_RESOURCE_BARRIER after[2] = {
        bridge::Transition(color, kRead, D3D12_RESOURCE_STATE_COPY_DEST),
        bridge::Transition(g_sharpenOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(2, after);
    if (ok) list->CopyResource(color, g_sharpenOutput.Get());
    const D3D12_RESOURCE_BARRIER back[2] = {
        bridge::Transition(color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
        bridge::Transition(g_sharpenOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, back);
}

// DLSS 5 and / or sharpening without the upscaler, on `color` (immediate context thread, between the
// command list halves): the scene copy at native resolution, or the game's image scaled to the output.
void RunPost(UpscaleInputs in, Dlss5Request nr, SharpenRequest sr, ID3D12Resource* color) {
    const uint64_t ready = bridge::SignalFrom11();
    bridge::WaitOn12(ready);
    ID3D12GraphicsCommandList* list = bridge::BeginList();
    const bool ok = nr.enabled && NeuralRender(list, color, in, nr.reset, nr.options);
    Sharpen(list, color, sr);
    bridge::Submit(list);
    bridge::WaitOn11(bridge::SignalFrom12());
    if (nr.enabled) Dlss5Ran(nr, ok);
}

// The upscaler on the D3D12 bridge, between the two halves of the frame (immediate context thread).
void RunUpscaler(UpscaleInputs in, Backend backend, uint32_t preset, Dlss5Request nr = {}, SharpenRequest sr = {}) {
    const uint64_t ready = bridge::SignalFrom11();
    bridge::WaitOn12(ready);
    ID3D12GraphicsCommandList* list = bridge::BeginList();
    in.list = list;
    const bool ok = upscaler::Evaluate(backend, in, preset);
    // DLSS 5 on the upscaled scene (the HUD is drawn over it afterwards).
    if (ok && nr.enabled) Dlss5Ran(nr, NeuralRender(list, in.output, in, nr.reset, nr.options));
    // Sharpening of the upscaled (display-referred) scene; the native path's output is HDR, sharpened at
    // the HUD instead (RunPost).
    if (ok && !in.colorHdr) Sharpen(list, in.output, sr);
    bridge::Submit(list);
    bridge::WaitOn11(bridge::SignalFrom12());
    g_lastRunOk = ok;
    if (ok) {
        g_ranAt = g_present.load();
        g_failures = 0;
    } else if (++g_failures >= 3) {
        g_failed = true;
        log::Error("Upscaler: {} failed 3 times in a row - off until re-enabled (F7)", ToString(backend));
    }
}

}  // namespace

void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext* immediate) {
    const auto& cfg = config::Get();
    {
        std::lock_guard lock(g_mutex);
        g_enabled = cfg.upscalerEnabled;
        g_backend = cfg.backend;
        g_dlssPreset = cfg.dlssPreset;
        g_fsrVersion = cfg.fsrVersion;
        g_flipX = cfg.jitterFlipX;
        g_flipY = cfg.jitterFlipY;
        g_dlss5Enabled = cfg.dlss5Enabled;
        g_dlss5.intensity = cfg.dlss5Intensity;
        g_dlss5.localTone = cfg.dlss5LocalTone;
        g_dlss5.localStructure = cfg.dlss5LocalStructure;
        g_dlss5.skinStructure = cfg.dlss5SkinStructure;
        g_dlss5.style = static_cast<uint32_t>(cfg.dlss5Style);
        g_sharpenFilter = cfg.sharpenFilter;
        g_sharpness = cfg.sharpness;
    }
    fsr::SetVersion(cfg.fsrVersion);
    d3d11::Initialize(device);
    const bool dlssFrameGen = cfg.presenter && cfg.frameGenerationBackend == Backend::Dlss;
    if (!bridge::Initialize(device, immediate, dlssFrameGen))
        log::Error("Upscaler / frame generation unavailable: the D3D12 bridge could not be created");
}

void OnVelocityDraw(ContextState& s) {
    g_velocitySeenAt = g_present.load(std::memory_order_relaxed);
    const std::vector<uint8_t>& data = s.vsConstants.data;
    if (s.vsConstants.source != s.vsCb0 || data.size() < 16 * 16) return;
    float r[16][4];
    std::memcpy(r, data.data(), sizeof(r));
    // c0-c3 jittered view-projection, c12-c15 unjittered (see the velocity vertex shaders).
    int k = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(r[15][i]) > std::fabs(r[15][k])) k = i;
    if (std::fabs(r[15][k]) < 1e-6f) return;
    std::lock_guard lock(g_mutex);
    g_camera.valid = true;
    g_camera.jitterNdc[0] = (r[0][k] - r[12][k]) / r[15][k];
    g_camera.jitterNdc[1] = (r[1][k] - r[13][k]) / r[15][k];
    std::memcpy(g_camera.viewProj, r[12], sizeof(float) * 16);
    std::memcpy(g_camera.prevViewProj, r[8], sizeof(float) * 16);
}

bool OnTaaDraw(ContextState& s, const std::function<void()>& draw) {
    const uint64_t present = g_present.load(std::memory_order_relaxed);
    g_taaSeenAt = present;
    const bool frameGen = framegen::Wanted();
    if ((!UpscalerWanted() && !frameGen && !g_dlss5Enabled && !SharpenWanted()) || !bridge::Ready()) return false;

    ID3D11ShaderResourceView* colorSrv = s.psSrv[0];
    ID3D11ShaderResourceView* depthSrv = s.psSrv[2];
    ID3D11ShaderResourceView* velocitySrv = s.psSrv[3];
    ID3D11RenderTargetView* outRtv = s.rtv[0];
    if (!colorSrv || !depthSrv || !outRtv) {
        g_reason = "TAA inputs not bound";
        return false;
    }
    const std::vector<uint8_t>& regs = s.psConstants.data;
    if (s.psConstants.source != s.psCb0 || regs.size() < kTaaRegisters * 16) {
        g_reason = "TAA constants not captured yet";
        return false;
    }

    ComPtr<ID3D11Resource> colorResource, outResource;
    colorSrv->GetResource(&colorResource);
    outRtv->GetResource(&outResource);
    ComPtr<ID3D11Texture2D> colorTexture, outTexture;
    if (FAILED(colorResource.As(&colorTexture)) || FAILED(outResource.As(&outTexture))) return false;
    D3D11_TEXTURE2D_DESC colorDesc, outDesc;
    colorTexture->GetDesc(&colorDesc);
    outTexture->GetDesc(&outDesc);
    D3D11_SHADER_RESOURCE_VIEW_DESC colorView;
    colorSrv->GetDesc(&colorView);
    D3D11_RENDER_TARGET_VIEW_DESC outView;
    outRtv->GetDesc(&outView);
    const UINT width = outDesc.Width, height = outDesc.Height;
    if (colorDesc.Width != width || colorDesc.Height != height || colorDesc.SampleDesc.Count != 1) {
        g_reason = "TAA input and output sizes differ";
        return false;
    }

    std::lock_guard lock(g_mutex);
    if (width != g_width || height != g_height) {
        log::Info("Upscaler: TAA resolve at {}x{} (color {}, output {})", width, height,
                  static_cast<int>(colorDesc.Format), static_cast<int>(outView.Format));
        g_width = width;
        g_height = height;
        g_resetNext = true;
    }
    using namespace bridge;
    // D3D12 -> D3D11 sharing refuses the game's R11G11B10 (E_INVALIDARG): color and output are shared as
    // RGBA16F, converted with full-screen copies on the D3D11 side.
    const DXGI_FORMAT outFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!EnsureShared(g_depth, width, height, DXGI_FORMAT_R32_FLOAT, kShaderResource | kUnorderedAccess,
                      L"DSRUpscaler depth") ||
        !EnsureShared(g_motion, width, height, DXGI_FORMAT_R16G16_FLOAT, kShaderResource | kUnorderedAccess,
                      L"DSRUpscaler motion vectors") ||
        !EnsureShared(g_color, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, kShaderResource | kRenderTarget,
                      L"DSRUpscaler color") ||
        !EnsureShared(g_output, width, height, outFormat, kShaderResource | kUnorderedAccess, L"DSRUpscaler output")) {
        g_reason = "shared textures could not be created";
        return false;
    }

    // Motion vectors and depth, from the resolve's constants.
    d3d11::MotionConstants mc{};
    Read4(regs, kRegScreenSize, mc.size);
    mc.size[0] = static_cast<float>(width);
    mc.size[1] = static_cast<float>(height);
    mc.size[2] = 1.0f / static_cast<float>(width);
    mc.size[3] = 1.0f / static_cast<float>(height);
    for (uint32_t i = 0; i < 4; ++i) {
        Read4(regs, kRegClipToWorld + i, mc.clipToWorld[i]);
        Read4(regs, kRegWorldToPrevClip + i, mc.worldToPrevClip[i]);
    }
    Read4(regs, kRegCameraOffset, mc.cameraOffset);
    mc.flags[0] = velocitySrv ? 1u : 0u;
    d3d11::DispatchMotionVectors(s.context, depthSrv, velocitySrv, g_motion.uav.Get(), g_depth.uav.Get(), mc);

    // Camera: planes from the resolve's depth linearization (w / (z * d + y): y = far, w = near * far).
    float depthParams[4];
    Read4(regs, kRegDepthParams, depthParams);
    if (depthParams[1] != 0) {
        g_far = depthParams[1];
        g_near = depthParams[3] / depthParams[1];
    }

    UpscaleInputs in;
    in.color = g_color.d3d12.Get();
    in.colorState = D3D12_RESOURCE_STATE_COMMON;
    in.depth = g_depth.d3d12.Get();
    in.depthState = D3D12_RESOURCE_STATE_COMMON;
    in.depthViewFormat = DXGI_FORMAT_R32_FLOAT;
    in.motionVectors = g_motion.d3d12.Get();
    in.motionVectorsState = D3D12_RESOURCE_STATE_COMMON;
    in.output = g_output.d3d12.Get();
    in.outputState = D3D12_RESOURCE_STATE_COMMON;
    in.renderSize = in.outputSize = {width, height};
    in.quality = QualityMode::Native;
    in.mvScaleX = in.mvScaleY = 1.0f;
    in.cameraNear = g_near;
    in.cameraFar = g_far;
    in.depthInverted = false;
    in.colorHdr = true;
    if (g_camera.valid) {
        // Projection shift in render pixels (+x right, +y down).
        in.jitterX = g_camera.jitterNdc[0] * static_cast<float>(width) * 0.5f * (g_flipX ? -1.0f : 1.0f);
        in.jitterY = -g_camera.jitterNdc[1] * static_cast<float>(height) * 0.5f * (g_flipY ? -1.0f : 1.0f);
        g_lastJitter[0] = in.jitterX;
        g_lastJitter[1] = in.jitterY;
        Decompose(g_camera.viewProj, g_near, g_far, in.projection, in.view, in.fovY, in.aspect);
        in.hasView = true;
        float prevFov = 0, prevAspect = 0;
        Decompose(g_camera.prevViewProj, g_near, g_far, in.prevProjection, in.prevView, prevFov, prevAspect);
        in.hasPrevious = true;
        g_fovY = in.fovY;
    } else {
        in.fovY = 0.9f;
        in.aspect = static_cast<float>(width) / static_cast<float>(height);
        in.projection[0] = in.projection[5] = in.projection[10] = in.projection[15] = 1.0f;
    }
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    in.frameTimeMs = g_lastTaaTime.QuadPart
                         ? static_cast<float>(static_cast<double>(now.QuadPart - g_lastTaaTime.QuadPart) * 1000.0 /
                                              static_cast<double>(frequency.QuadPart))
                         : 16.7f;
    g_lastTaaTime = now;
    in.reset = g_resetNext;

    const Backend backend = g_backend;
    if (UpscalerWanted() && !upscaler::Available(backend)) g_reason = upscaler::Probed(backend) ? "backend unavailable" : "loading";

    // The game renders below the output size (its resolution setting under the screen's): the jittered
    // scene goes through post-processing unresolved, and is upscaled at the first HUD draw.
    UINT gameW = 0, gameH = 0, outW = 0, outH = 0;
    const presenter::Rect rect = presenter::OutputRect();
    if (presenter::Sizes(gameW, gameH, outW, outH)) {
        outW = rect.width;  // the game's image area in the output (its aspect ratio)
        outH = rect.height;
    }
    const bool below = outW && outH && (outW > width + 4 || outH > height + 4);
    if (below && UpscalerWanted() && upscaler::Available(backend)) {
        draw();  // the resolve's other outputs (the next frame's inputs) stay the game's
        d3d11::Blit(s.context, colorSrv, outRtv, width, height);  // its image: the jittered scene
        in.outputSize = {outW, outH};
        in.quality = ModeForRatio(backend, static_cast<float>(width) / static_cast<float>(outW));
        in.colorHdr = false;  // upscaled after tone mapping
        g_pending = in;
        g_pendingValid = true;
        g_pendingScaleOnly = false;
        g_outputWidth = outW;
        g_outputHeight = outH;
        g_activeMode = in.quality;
        g_split = s.deferred;
        g_resetNext = false;
        g_reason = "";
        return true;
    }
    g_outputWidth = width;
    g_outputHeight = height;
    g_activeMode = QualityMode::Native;
    // Upscaler off, DLSS 5 or sharpening on, the game below the screen: the game's TAA image is scaled to
    // the output (bilinear) at the first HUD draw and they run there, as after the upscaler. DLSS 5 at the
    // game's resolution hung the GPU within seconds (DXGI_ERROR_DEVICE_HUNG, 2560x1440 on 3840x2160).
    if (below && !UpscalerWanted() && (Dlss5Now().enabled || SharpenWanted())) {
        draw();
        in.outputSize = {outW, outH};
        g_pending = in;
        g_pendingValid = true;
        g_pendingScaleOnly = true;
        g_split = s.deferred;
        return true;
    }
    const bool upscale = UpscalerWanted() && upscaler::Available(backend) && g_color.rtv != nullptr;
    g_nativePending = in;
    g_nativePendingValid = g_dlss5Enabled || SharpenWanted();
    if (upscale) d3d11::Blit(s.context, colorSrv, g_color.rtv.Get(), width, height);

    draw();  // the game's resolve: its history / other outputs stay the game's

    if (upscale) {
        g_resetNext = false;
        g_reason = "";
        g_split = s.deferred;
        const uint32_t preset = g_dlssPreset;
        hooks::d3d11::SplitHere(s, [in, backend, preset] { RunUpscaler(in, backend, preset); });
        if (!d3d11::TryCopy(s.context, outResource.Get(), g_output.d3d11.Get()))
            d3d11::Blit(s.context, g_output.srv.Get(), outRtv, width, height);
    }
    if (frameGen)
        hooks::d3d11::AtExecute(s, [in] {
            std::lock_guard frameLock(g_mutex);
            g_frame.inputs = in;
            g_frame.hasInputs = true;
        });
    return true;
}


// (Re)creates `texture` / `srv` as a D3D11-only copy target like `desc` (shader resource only).
bool EnsureCopy(ID3D11DeviceContext* context, ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& srv,
                const D3D11_TEXTURE2D_DESC& like, DXGI_FORMAT viewFormat) {
    D3D11_TEXTURE2D_DESC current{};
    if (texture) texture->GetDesc(&current);
    if (texture && current.Width == like.Width && current.Height == like.Height && current.Format == like.Format)
        return true;
    D3D11_TEXTURE2D_DESC copy = like;
    copy.MipLevels = 1;
    copy.ArraySize = 1;
    copy.SampleDesc = {1, 0};
    copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy.MiscFlags = 0;
    copy.CPUAccessFlags = 0;
    copy.Usage = D3D11_USAGE_DEFAULT;
    texture.Reset();
    srv.Reset();
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = viewFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    return SUCCEEDED(device->CreateTexture2D(&copy, nullptr, &texture)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv));
}

// Upscaled frame, first HUD draw: the upscaler runs (split), then the HUD is drawn over its image at
// the output size.
void UpscaleAtHud(ContextState& s, ID3D11Resource* target, const D3D11_TEXTURE2D_DESC& desc) {
    UpscaleInputs in = g_pending;
    g_pendingValid = false;
    const bool scaleOnly = g_pendingScaleOnly;
    const UINT outW = in.outputSize.width, outH = in.outputSize.height;
    if (desc.Width != in.renderSize.width || desc.Height != in.renderSize.height) return;
    using namespace bridge;
    // The scene as RGBA16F: the upscalers get a typed format whatever the game's target is (typeless / sRGB).
    ComPtr<ID3D11ShaderResourceView> sceneSrv;
    {
        ComPtr<ID3D11Device> device;
        s.context->GetDevice(&device);
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        s.rtv[0]->GetDesc(&rtvDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = rtvDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(target, &srvDesc, &sceneSrv);
    }
    if (!sceneSrv ||
        (!scaleOnly && !EnsureShared(g_sceneLdr, desc.Width, desc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     kShaderResource | kRenderTarget, L"DSRUpscaler scene (LDR)")) ||
        !EnsureShared(g_upscaled, outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT, kShaderResource | kRenderTarget | kUnorderedAccess,
                      L"DSRUpscaler upscaled scene")) {
        g_reason = "upscaling textures could not be created";
        return;
    }
    in.output = g_upscaled.d3d12.Get();
    const Dlss5Request nr = Dlss5Now();
    const SharpenRequest sr = SharpenNow();
    if (scaleOnly) {
        d3d11::Blit(s.context, sceneSrv.Get(), g_upscaled.rtv.Get(), outW, outH);
        ID3D12Resource* scaled = g_upscaled.d3d12.Get();
        hooks::d3d11::SplitHere(s, [in, nr, sr, scaled] { RunPost(in, nr, sr, scaled); });
    } else {
        d3d11::Blit(s.context, sceneSrv.Get(), g_sceneLdr.rtv.Get(), desc.Width, desc.Height);
        in.color = g_sceneLdr.d3d12.Get();
        const Backend backend = g_backend;
        const uint32_t preset = g_dlssPreset;
        hooks::d3d11::SplitHere(s, [in, backend, preset, nr, sr] { RunUpscaler(in, backend, preset, nr, sr); });
    }
    // The HUD-less upscaled scene for frame generation, before the HUD is drawn over it.
    const bool frameGen = framegen::Wanted();
    if (frameGen) {
        D3D11_TEXTURE2D_DESC upscaledDesc;
        g_upscaled.d3d11->GetDesc(&upscaledDesc);
        if (EnsureCopy(s.context, g_scenePre, g_scenePreSrv, upscaledDesc, DXGI_FORMAT_R16G16B16A16_FLOAT))
            d3d11::TryCopy(s.context, g_scenePre.Get(), g_upscaled.d3d11.Get());
    }
    hooks::d3d11::BeginRedirect(s, s.rtv[0], g_upscaled.rtv.Get(), static_cast<float>(outW) / desc.Width,
                                static_cast<float>(outH) / desc.Height);
    hooks::d3d11::AtExecute(s, [in, frameGen, scaleOnly] {
        std::lock_guard frameLock(g_mutex);
        // The upscaler failed: the presenter shows the game's image (stretched) instead of a black one.
        g_upscaledReady = scaleOnly || g_lastRunOk.load();
        if (frameGen) {
            g_scenePreReady = true;
            g_frame.inputs = in;
            g_frame.hasInputs = true;
        }
    });
}

void OnHudDraw(ContextState& s) {
    if (!bridge::Ready() || !s.rtv[0]) return;
    ComPtr<ID3D11Resource> target;
    s.rtv[0]->GetResource(&target);
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(target.As(&texture))) return;
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    std::lock_guard lock(g_mutex);
    if (g_pendingValid) return UpscaleAtHud(s, target.Get(), desc);
    if (g_nativePendingValid) {
        g_nativePendingValid = false;
        const Dlss5Request nr = Dlss5Now();
        const SharpenRequest sr = SharpenNow();
        if ((nr.enabled || sr.Wanted()) && desc.Width == g_nativePending.renderSize.width &&
            desc.Height == g_nativePending.renderSize.height &&
            // Unordered access too, like the upscaled scene DLSS 5 gets in the upscaling path.
            bridge::EnsureShared(g_nrScene, desc.Width, desc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 bridge::kShaderResource | bridge::kRenderTarget | bridge::kUnorderedAccess,
                                 L"DSRUpscaler DLSS 5 scene")) {
            // The scene (LDR) as RGBA16F for DLSS 5 / sharpening, and the result back into the scene target.
            ComPtr<ID3D11ShaderResourceView> sceneSrv;
            ComPtr<ID3D11Device> device;
            s.context->GetDevice(&device);
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
            s.rtv[0]->GetDesc(&rtvDesc);
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = rtvDesc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            if (SUCCEEDED(device->CreateShaderResourceView(target.Get(), &srvDesc, &sceneSrv))) {
                ID3D11RenderTargetView* sceneTarget = s.rtv[0];
                d3d11::Blit(s.context, sceneSrv.Get(), g_nrScene.rtv.Get(), desc.Width, desc.Height);
                const UpscaleInputs in = g_nativePending;
                ID3D12Resource* scene = g_nrScene.d3d12.Get();
                hooks::d3d11::SplitHere(s, [in, nr, sr, scene] { RunPost(in, nr, sr, scene); });
                d3d11::Blit(s.context, g_nrScene.srv.Get(), sceneTarget, desc.Width, desc.Height);
            }
        }
    }
    if (!framegen::Wanted()) return;
    // The HUD drawn over the scene at the output size: anything else is an offscreen UI texture.
    if (desc.Width != g_width || desc.Height != g_height) return;
    D3D11_TEXTURE2D_DESC current{};
    if (g_scenePre) g_scenePre->GetDesc(&current);
    if (!g_scenePre || current.Width != desc.Width || current.Height != desc.Height || current.Format != desc.Format) {
        D3D11_TEXTURE2D_DESC copy = desc;
        copy.MipLevels = 1;
        copy.ArraySize = 1;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy.MiscFlags = 0;
        copy.CPUAccessFlags = 0;
        copy.Usage = D3D11_USAGE_DEFAULT;
        g_scenePre.Reset();
        g_scenePreSrv.Reset();
        ComPtr<ID3D11Device> device;
        s.context->GetDevice(&device);
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        s.rtv[0]->GetDesc(&rtvDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = rtvDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateTexture2D(&copy, nullptr, &g_scenePre)) ||
            FAILED(device->CreateShaderResourceView(g_scenePre.Get(), &srvDesc, &g_scenePreSrv)))
            return;
    }
    if (!d3d11::TryCopy(s.context, g_scenePre.Get(), target.Get())) return;
    hooks::d3d11::AtExecute(s, [] {
        std::lock_guard frameLock(g_mutex);
        g_scenePreReady = true;
    });
}

// The game's final pass (brightness) once more, with `source` into `target` at width x height.
void DrawFinalPass(ContextState& s, const std::function<void()>& draw, ID3D11ShaderResourceView* source,
                   ID3D11RenderTargetView* target, UINT x, UINT y, UINT width, UINT height) {
    ID3D11ShaderResourceView* original = s.psSrv[0];
    ID3D11RenderTargetView* targets[8];
    std::memcpy(targets, s.rtv, sizeof(targets));
    const UINT targetCount = s.rtvCount;
    ID3D11DepthStencilView* depth = s.dsv;
    const bool hadViewport = s.hasViewport;
    const D3D11_VIEWPORT viewport = s.viewport;
    const D3D11_VIEWPORT full{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                              static_cast<float>(height), 0, 1};
    s.context->PSSetShaderResources(0, 1, &source);
    s.context->OMSetRenderTargets(1, &target, nullptr);
    s.context->RSSetViewports(1, &full);
    draw();
    s.context->OMSetRenderTargets(targetCount, targets, depth);
    s.context->PSSetShaderResources(0, 1, &original);
    if (hadViewport) s.context->RSSetViewports(1, &viewport);
}

bool OnGammaDraw(ContextState& s, const std::function<void()>& draw) {
    {
        std::lock_guard lock(g_mutex);
        if (g_upscaledReady) {
            g_upscaledReady = false;
            UINT gameW = 0, gameH = 0, outW = 0, outH = 0;
            ID3D11RenderTargetView* final = presenter::FinalTarget();
            const presenter::Rect rect = presenter::OutputRect();
            if (final && presenter::Sizes(gameW, gameH, outW, outH) && g_upscaled.srv) {
                DrawFinalPass(s, draw, g_upscaled.srv.Get(), final, rect.x, rect.y, rect.width, rect.height);
                if (g_scenePreReady && g_scenePreSrv && framegen::Wanted()) {
                    ComPtr<ID3D11Resource> finalResource;
                    final->GetResource(&finalResource);
                    ComPtr<ID3D11Texture2D> finalTexture;
                    D3D11_TEXTURE2D_DESC finalDesc{};
                    if (SUCCEEDED(finalResource.As(&finalTexture))) finalTexture->GetDesc(&finalDesc);
                    if (bridge::EnsureShared(g_hudless, outW, outH, finalDesc.Format,
                                             bridge::kShaderResource | bridge::kRenderTarget, L"DSRUpscaler HUD-less")) {
                        const float black[4] = {0, 0, 0, 1};
                        s.context->ClearRenderTargetView(g_hudless.rtv.Get(), black);
                        DrawFinalPass(s, draw, g_scenePreSrv.Get(), g_hudless.rtv.Get(), rect.x, rect.y, rect.width,
                                      rect.height);
                        g_frame.hasHudless = true;
                    }
                }
                g_scenePreReady = false;
                presenter::MarkFinal();
            }
        }
        const bool wanted = g_scenePreReady && g_scenePreSrv && framegen::Wanted() && s.rtv[0];
        g_scenePreReady = false;
        if (wanted) {
            ComPtr<ID3D11Resource> backBuffer;
            s.rtv[0]->GetResource(&backBuffer);
            ComPtr<ID3D11Texture2D> texture;
            D3D11_TEXTURE2D_DESC desc{};
            if (SUCCEEDED(backBuffer.As(&texture))) texture->GetDesc(&desc);
            if (desc.Width &&
                bridge::EnsureShared(g_hudless, desc.Width, desc.Height, desc.Format,
                                     bridge::kShaderResource | bridge::kRenderTarget, L"DSRUpscaler HUD-less")) {
                // The same draw with the HUD-less scene as its source, into the HUD-less target.
                ID3D11ShaderResourceView* source = s.psSrv[0];
                ID3D11RenderTargetView* targets[8];
                std::memcpy(targets, s.rtv, sizeof(targets));
                const UINT targetCount = s.rtvCount;
                ID3D11DepthStencilView* depth = s.dsv;
                ID3D11ShaderResourceView* hudless = g_scenePreSrv.Get();
                ID3D11RenderTargetView* hudlessTarget = g_hudless.rtv.Get();
                s.context->PSSetShaderResources(0, 1, &hudless);
                s.context->OMSetRenderTargets(1, &hudlessTarget, nullptr);
                draw();
                s.context->OMSetRenderTargets(targetCount, targets, depth);
                s.context->PSSetShaderResources(0, 1, &source);
                g_frame.hasHudless = true;
            }
        }
    }
    draw();
    return true;
}

bool TakeFrameInputs(UpscaleInputs& inputs, ID3D12Resource*& hudless) {
    std::lock_guard lock(g_mutex);
    const bool ok = g_frame.hasInputs && g_frame.hasHudless;
    if (ok) {
        inputs = g_frame.inputs;
        hudless = g_hudless.d3d12.Get();
    }
    g_frame.hasInputs = g_frame.hasHudless = false;
    return ok;
}

void OnPresent() {
    const uint64_t present = ++g_present;
    if (ContextState* immediate = hooks::d3d11::ImmediateState()) hooks::d3d11::EndRedirect(*immediate);
    // F7: upscaler on / off (game window focused).
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(config::Get().upscalerKey) & 0x8000);
    if (down && !g_keyDown) SetEnabled(!g_enabled);
    g_keyDown = down;
    const bool dlss5Down = pid == GetCurrentProcessId() && (GetAsyncKeyState(config::Get().dlss5Key) & 0x8000);
    if (dlss5Down && !g_dlss5KeyDown) SetDlss5(!g_dlss5Enabled, g_dlss5.intensity, static_cast<int>(g_dlss5.style));
    g_dlss5KeyDown = dlss5Down;

    if (g_enabled) upscaler::Prepare(g_backend);
    upscaler::OnPresent();
    if (present - g_taaSeenAt.load() > 5) g_reason = "the game's TAA is not running (Anti-aliasing option, menus)";
}

Status GetStatus() {
    std::lock_guard lock(g_mutex);
    Status s;
    const uint64_t present = g_present.load();
    s.enabled = g_enabled;
    s.backend = g_backend;
    s.quality = g_activeMode;
    s.outputWidth = g_outputWidth;
    s.outputHeight = g_outputHeight;
    s.available = upscaler::Available(g_backend);
    s.probed = upscaler::Probed(g_backend);
    s.active = g_enabled && present - g_ranAt.load() < 10 && g_ranAt.load() != 0;
    s.failed = g_failed;
    s.reason = s.active ? "" : g_reason;
    s.description = upscaler::Description(g_backend);
    s.width = g_width;
    s.height = g_height;
    s.dlssPreset = g_dlssPreset;
    s.fsrVersion = g_fsrVersion;
    s.taaSeen = present - g_taaSeenAt.load() < 10 && g_taaSeenAt.load() != 0;
    s.velocitySeen = present - g_velocitySeenAt.load() < 10 && g_velocitySeenAt.load() != 0;
    s.splitLists = g_split;
    s.jitterX = g_lastJitter[0];
    s.jitterY = g_lastJitter[1];
    s.flipX = g_flipX;
    s.flipY = g_flipY;
    s.nearPlane = g_near;
    s.farPlane = g_far;
    s.fovY = g_fovY;
    s.dlss5Enabled = g_dlss5Enabled;
    s.dlss5Active = g_dlss5RanAt.load() && present - g_dlss5RanAt.load() <= 3;
    s.dlss5Reason = upscaler::dlss_nr::UnavailableReason();
    s.dlss5Intensity = g_dlss5.intensity;
    s.dlss5Style = static_cast<int>(g_dlss5.style);
    s.sharpenFilter = g_sharpenFilter;
    s.sharpness = g_sharpness;
    s.nisAvailable = streamline::NisAvailable();
    return s;
}

void SetEnabled(bool enabled) {
    {
        std::lock_guard lock(g_mutex);
        g_enabled = enabled;
        g_resetNext = true;
        if (enabled) g_failed = false, g_failures = 0;
    }
    log::Info("Upscaler {}", enabled ? "on" : "off (the game's TAA)");
    config::SaveUpscaler(enabled, g_backend, config::Get().quality);
}

void SetBackend(Backend backend) {
    {
        std::lock_guard lock(g_mutex);
        g_backend = backend;
        g_failed = false;
        g_failures = 0;
        g_resetNext = true;
    }
    log::Info("Upscaler backend: {}", ToString(backend));
    config::SaveUpscaler(g_enabled, backend, config::Get().quality);
}

void SetDlssPreset(uint32_t preset) {
    {
        std::lock_guard lock(g_mutex);
        g_dlssPreset = preset;
    }
    config::SaveDlssPreset(preset);
}

void SetFsrVersion(const std::string& version) {
    {
        std::lock_guard lock(g_mutex);
        g_fsrVersion = version;
    }
    fsr::SetVersion(version);
    config::SaveFsrVersion(version);
}

void SetJitterFlip(bool x, bool y) {
    {
        std::lock_guard lock(g_mutex);
        g_flipX = x;
        g_flipY = y;
        g_resetNext = true;
    }
    config::SaveJitter(x, y);
}

void SetDlss5(bool enabled, float intensity, int style) {
    {
        std::lock_guard lock(g_mutex);
        if (enabled != g_dlss5Enabled) g_dlss5Reset = true;
        g_dlss5Enabled = enabled;
        g_dlss5.intensity = std::clamp(intensity, 0.0f, 2.0f);
        g_dlss5.style = static_cast<uint32_t>(std::clamp(style, 0, 7));
    }
    log::Info("DLSS 5: {} (intensity {:.2f}, style {})", enabled ? "on" : "off", intensity, style);
    config::SaveDlss5(enabled, intensity, style);
}

void SetSharpen(SharpenFilter filter, float sharpness) {
    sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    {
        std::lock_guard lock(g_mutex);
        g_sharpenFilter = filter;
        g_sharpness = sharpness;
    }
    config::SaveSharpen(filter, sharpness);
}

void Shutdown() {
    {
        std::lock_guard lock(g_mutex);
        g_enabled = false;
        g_dlss5Enabled = false;
    }
    if (bridge::Ready()) {
        bridge::WaitIdle();
        upscaler::dlss_nr::Shutdown(bridge::Device12());
    }
}

}  // namespace dsru::render::upscale
