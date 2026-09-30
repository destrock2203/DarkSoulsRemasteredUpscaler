#pragma once

#include <d3d12.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace dsru {

enum class QualityMode { UltraPerformance, Performance, Balanced, Quality, UltraQuality, Native };

// The upscaler that replaces the game's TAA resolve.
enum class Backend : uint8_t { Dlss, Fsr, Xess };
inline constexpr Backend kBackends[] = {Backend::Dlss, Backend::Fsr, Backend::Xess};

struct Resolution {
    uint32_t width = 0;
    uint32_t height = 0;
};

// A region of a texture, in pixels.
struct Rect {
    uint32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

const char* ToString(QualityMode mode);
QualityMode ParseQualityMode(std::string_view text, QualityMode fallback);

const char* ToString(Backend backend);  // "DLSS", "FSR", "XeSS"
Backend ParseBackend(std::string_view text, Backend fallback);

// The modes a backend offers, best quality first (Native: DLAA / FSR Native AA / XeSS AA).
std::span<const QualityMode> Modes(Backend backend);
// `mode`, or the closest one `backend` offers.
QualityMode SupportedMode(Backend backend, QualityMode mode);
// Render / output size ratio of a mode, per dimension, as each vendor specifies it (DLSS's actual
// size comes from Streamline and can differ slightly).
float NominalScale(Backend backend, QualityMode mode);
// The mode to evaluate with for a render / output ratio. XeSS modes are ranges (1x up to the
// mode's factor): its mode is the best one whose range still covers the ratio.
QualityMode ModeForRatio(Backend backend, float ratio);
// Display name: "DLAA", "FSR Native AA", "Quality", ...
const char* ModeName(Backend backend, QualityMode mode);

// The sharpening filter on the upscaler's image: the game's own, AMD FidelityFX RCAS or NVIDIA
// Image Scaling (render/sharpen.h, streamline::EvaluateNis). XeSS has no sharpening of its own.
enum class SharpenFilter : uint8_t { Game, Amd, Nvidia };
inline constexpr SharpenFilter kSharpenFilters[] = {SharpenFilter::Game, SharpenFilter::Amd, SharpenFilter::Nvidia};
const char* ToString(SharpenFilter filter);  // ini value: "Game", "AMD", "NVIDIA"
SharpenFilter ParseSharpenFilter(std::string_view text, SharpenFilter fallback);

// DLSS presets (sl::DLSSPreset values): 0 = NVIDIA's default for each mode.
struct DlssPresetInfo {
    uint32_t value;
    const char* name;         // ini value
    const char* description;  // for the overlay
};
std::span<const DlssPresetInfo> DlssPresets();
const char* DlssPresetName(uint32_t value);
uint32_t ParseDlssPreset(std::string_view text, uint32_t fallback);

// One upscaler evaluation, recorded into `list`. States are the ones each resource is in when the
// evaluation executes; the backend returns every resource to it.
struct UpscaleInputs {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* color = nullptr;  // render resolution, HDR, jittered
    D3D12_RESOURCE_STATES colorState{};
    ID3D12Resource* depth = nullptr;
    D3D12_RESOURCE_STATES depthState{};
    DXGI_FORMAT depthViewFormat = DXGI_FORMAT_UNKNOWN;  // SRV format for a typeless depth resource
    ID3D12Resource* motionVectors = nullptr;
    D3D12_RESOURCE_STATES motionVectorsState{};
    ID3D12Resource* output = nullptr;  // output resolution, must allow UAV
    D3D12_RESOURCE_STATES outputState{};
    Resolution renderSize{};
    Resolution outputSize{};
    // Frame generation: the part of the back buffer (outputSize) the scene covers, when the game
    // letterboxes it (a 16:9 image on a 16:10 screen). Empty: all of it.
    Rect outputRect{};
    QualityMode quality = QualityMode::Native;

    float jitterX = 0, jitterY = 0;  // the projection's shift, render pixels (+x right, +y down)
    // Motion vectors * mvScale = motion to the previous frame in render-extent UV units ([0, 1]
    // across the render size, Streamline's convention). FSR / XeSS scale by the render size.
    float mvScaleX = 1, mvScaleY = 1;
    // Unjittered projection, row-major for column vectors (clip = M * view), as the game stores it.
    float projection[16]{};
    float cameraNear = 0, cameraFar = 0, fovY = 0, aspect = 0;
    // World -> view (row-major for column vectors, as the game stores it), and the previous frame's
    // view and unjittered projection, for the reprojection matrices frame generation needs. Without
    // them the constants describe a static camera (enough for upscaling).
    bool hasView = false;
    float view[16]{};
    bool hasPrevious = false;
    float prevView[16]{};
    float prevProjection[16]{};
    bool depthInverted = false;  // Dark Souls Remastered: standard depth (1 = far)
    bool colorHdr = true;  // false: display-referred (tonemapped, gamma-encoded) color
    bool reset = false;
    float frameTimeMs = 16.7f;
};

}  // namespace dsru
