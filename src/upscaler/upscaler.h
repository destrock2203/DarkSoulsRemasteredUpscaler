#pragma once

#include "upscaler/upscaler_types.h"

#include <d3d12.h>

#include <string>

// The upscaler backends behind one interface: DLSS (upscaler/streamline), FSR (upscaler/fsr),
// XeSS (upscaler/xess). Streamline loads at the game's first device creation (it must hook the
// swapchain); FSR and XeSS load the first time they are selected.
namespace dsru::upscaler {

// From the D3D12CreateDevice hook, after Streamline's.
void OnDeviceCreated(ID3D12Device* device);

// Loads `backend`'s runtime if it isn't yet (the first call can take a moment). Present thread.
void Prepare(Backend backend);
// Loaded and supported on the game's GPU.
bool Available(Backend backend);
// A load was attempted (Prepare), so !Available means unavailable rather than not loaded yet.
bool Probed(Backend backend);
bool AnyAvailable();

// "DLSS", "FSR 3.1.5", "XeSS 3.0.2": the runtime version when known.
std::string Description(Backend backend);

// The engine render scale for `mode` at an output size (1 = native).
float RenderScale(Backend backend, Resolution output, QualityMode mode);

// One evaluation with `backend` (see UpscaleInputs). `dlssPreset`: sl::DLSSPreset value.
// Returns false on error (logged). Command list state is NOT restored here.
bool Evaluate(Backend backend, const UpscaleInputs& in, uint32_t dlssPreset);

// Once per present (retired FSR / XeSS contexts).
void OnPresent();

}  // namespace dsru::upscaler
