#pragma once

#include "upscaler/upscaler_types.h"

#include <d3d11.h>
#include <d3d12.h>

#include <functional>
#include <string>

namespace dsru::hooks::d3d11 {
struct ContextState;
}

// The upscaler in Dark Souls Remastered's frame (see game/dsr.h for the passes).
//  - velocity pass: the vertex constants hold the jittered view-projection (c0-c3), this frame's
//    unjittered one (c12-c15) and the previous frame's (c8-c11): jitter and camera.
//  - TAA resolve (FRPG_Fil_ResolveTAA): full motion vectors and depth are rebuilt into textures shared
//    with D3D12 (shaders/motion_vectors.hlsl, the resolve's own math), the jittered color is copied;
//    the game's resolve runs as usual (its other outputs feed the next frame), then the upscaler's
//    image (DLAA / FSR Native AA / XeSS AA, on the D3D12 bridge) replaces its result.
//  - first HUD draw: the scene is complete, the HUD-less image is copied for frame generation.
// When the resolve is recorded on a deferred context, its command list is split around the upscaler
// (hooks/d3d11_hooks.h, SplitHere).
namespace dsru::render::upscale {

void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext* immediate);

// From the draw hooks. OnTaaDraw issues the draw itself (`draw`) and returns true when it handled it.
bool OnTaaDraw(hooks::d3d11::ContextState& state, const std::function<void()>& draw);
void OnVelocityDraw(hooks::d3d11::ContextState& state);
void OnHudDraw(hooks::d3d11::ContextState& state);
// The game's final pass (scene + HUD -> back buffer with its brightness): the HUD-less copy is drawn
// through it too, so frame generation compares like with like. Issues `draw`; returns true.
bool OnGammaDraw(hooks::d3d11::ContextState& state, const std::function<void()>& draw);

// Present thread, before presenting: hotkeys, retired contexts.
void OnPresent();

// This frame's frame generation inputs, once its command lists have executed: depth, motion vectors
// (render size) and the HUD-less image (the back buffer's size), all D3D12 resources in
// D3D12_RESOURCE_STATE_COMMON, and the camera. False when the frame had none (menus, loading screens,
// the game's TAA off). Resets them for the next frame.
bool TakeFrameInputs(UpscaleInputs& inputs, ID3D12Resource*& hudless);

struct Status {
    bool enabled = false;         // user setting (F7)
    Backend backend = Backend::Dlss;
    QualityMode quality = QualityMode::Native;
    bool available = false;       // backend loaded and supported
    bool probed = false;
    bool active = false;          // ran in the last frames
    bool failed = false;          // stopped after an error
    const char* reason = "";      // why it isn't active
    std::string description;      // "DLSS", "FSR 3.1.5", ...
    uint32_t width = 0, height = 0;             // render size (the game's resolution)
    uint32_t outputWidth = 0, outputHeight = 0; // the upscaler's output (the screen)
    uint32_t dlssPreset = 0;
    std::string fsrVersion;
    bool taaSeen = false;         // the game's TAA resolve ran recently
    bool velocitySeen = false;
    bool splitLists = false;      // the resolve is recorded on a deferred context
    float jitterX = 0, jitterY = 0;
    bool flipX = false, flipY = false;
    float nearPlane = 0, farPlane = 0, fovY = 0;
    // DLSS 5 neural rendering (upscaler/dlss_nr)
    bool dlss5Enabled = false;      // user setting (F8)
    bool dlss5Active = false;       // ran in the last few frames
    const char* dlss5Reason = "";   // why it is unavailable, "" if not known to be
    float dlss5Intensity = 1.0f;
    int dlss5Style = 1;
};
Status GetStatus();

void SetEnabled(bool enabled);
void SetBackend(Backend backend);
void SetDlssPreset(uint32_t preset);
void SetFsrVersion(const std::string& version);
void SetJitterFlip(bool x, bool y);
// DLSS 5 on the HUD-less scene. Saved to the ini.
void SetDlss5(bool enabled, float intensity, int style);

void Shutdown();

}  // namespace dsru::render::upscale
