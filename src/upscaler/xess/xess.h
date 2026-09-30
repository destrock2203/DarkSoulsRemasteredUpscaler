#pragma once

#include "upscaler/upscaler_types.h"

#include <d3d12.h>

#include <string>

// Intel XeSS super resolution (libxess.dll, loaded dynamically). Runs on Intel Arc (XMX) and on
// other GPUs with Shader Model 6.4 + DP4a. Built as no-ops without the SDK (DSRU_HAS_XESS).
//
// Not thread-safe: apart from Available, calls come with render::upscale's lock held.
namespace dsru::xess {

// The game's device (from the D3D12CreateDevice hook). Nothing is loaded yet.
void OnDeviceCreated(ID3D12Device* device);
// Loads libxess.dll and checks this device is supported, once. Called when XeSS gets selected.
void Load();
bool Available();
// "XeSS 3.0.2" once loaded.
std::string VersionName();

// Inputs in PIXEL | NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS (upscaler::Evaluate
// guarantees it). in.quality must cover the render / output ratio (ModeForRatio). Returns false on
// error (logged). Command list state is NOT restored here.
bool Evaluate(const UpscaleInputs& in);

// Once per present: destroys contexts retired a few frames ago (GPU done with them).
void OnPresent();

}  // namespace dsru::xess
