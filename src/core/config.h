#pragma once

#include "core/log.h"
#include "upscaler/upscaler_types.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace dsru::config {

struct Config {
    // [General]
    bool enabled = true;
    log::Level logLevel = log::Level::Info;

    // [Loader]
    std::wstring chainLoadDxgi;
    // Load upscaler / frame generation DLLs whose signature is missing or broken (modified builds).
    bool allowUnsignedDlls = true;

    // [Upscaler] - also written by the overlay
    bool upscalerEnabled = true;  // replaces the game's TAA resolve (F7 toggles)
    Backend backend = Backend::Dlss;
    QualityMode quality = QualityMode::Native;  // Native = DLAA / native AA
    uint32_t dlssPreset = 0;  // sl::DLSSPreset value, 0 = NVIDIA's default per mode
    std::string fsrVersion;   // upscaler version name ("3.1.5", ...), empty = the newest available
    // Jitter conventions, in case the game's differ from what the mod assumes (overlay diagnostics).
    bool jitterFlipX = false;
    bool jitterFlipY = false;
    // The game's resolution below the monitor's in a window: the window covers the monitor so the
    // upscaler fills the screen.
    bool fillScreen = true;

    // [DLSS5] - neural rendering (upscaler/dlss_nr) of the HUD-less scene, also written by the overlay
    bool dlss5Enabled = false;
    int dlss5Key = 0x77;  // VK_F8
    float dlss5Intensity = 1.0f;
    float dlss5LocalTone = 0.5f;
    float dlss5LocalStructure = 1.0f;
    float dlss5SkinStructure = -1.0f;
    int dlss5Style = 1;

    // [FrameGeneration] - also written by the overlay
    // Backend: owns the swapchain, chosen at startup.
    Backend frameGenerationBackend = Backend::Dlss;
    bool frameGeneration = false;
    int generatedFrames = 1;  // 1 = 2x, 2 = 3x, 3 = 4x
    // The game gets a stand-in swapchain presented through D3D12, so frame generation can be turned on
    // in game. 0 = the game keeps its own D3D11 swapchain: no frame generation.
    bool presenter = true;

    // [Game]
    // Presenter: -1 = the game's V-Sync (on), 0 = off (tearing / G-Sync / FreeSync, the engine paces the
    // frames), 1 = on. The game's fullscreen would switch the display to 60 Hz; the presenter's borderless
    // window keeps the desktop rate, where 60 FPS with V-Sync judders.
    int vsync = 0;

    // [Overlay]
    int overlayKey = 0x24;  // VK_HOME
    bool overlayStatusLine = true;
    int upscalerKey = 0x76;  // VK_F7

    // [Debug]
    bool logResources = false;
};

// Loads the INI; missing file or keys fall back to defaults.
void Load(const std::filesystem::path& file);
const Config& Get();

// Persist one setting to the loaded ini and update Get().
void SaveUpscaler(bool enabled, Backend backend, QualityMode quality);
void SaveDlssPreset(uint32_t preset);
void SaveFsrVersion(const std::string& version);
void SaveJitter(bool flipX, bool flipY);
void SaveFrameGeneration(bool enabled, int generatedFrames);
void SaveFrameGenerationBackend(Backend backend);  // applies at the next launch
void SaveOverlayStatusLine(bool visible);
void SaveDlss5(bool enabled, float intensity, int style);

}  // namespace dsru::config
