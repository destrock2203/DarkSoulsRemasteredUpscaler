#include "core/config.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <format>

namespace dsru::config {

namespace {

Config g_config;
std::filesystem::path g_file;

std::wstring ReadString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[1024];
    GetPrivateProfileStringW(section, key, fallback, buffer, static_cast<DWORD>(std::size(buffer)), g_file.c_str());
    return buffer;
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, g_file.c_str()) != 0;
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback) {
    const std::wstring text = ReadString(section, key, L"");
    if (text.empty()) return fallback;
    wchar_t* end = nullptr;
    const float value = std::wcstof(text.c_str(), &end);
    return end == text.c_str() ? fallback : value;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_file.c_str()));
}

// "F1".."F24", a decimal or 0x-prefixed virtual-key code.
int ReadKey(const wchar_t* section, const wchar_t* key, int fallback) {
    const std::wstring text = ReadString(section, key, L"");
    if (text.empty()) return fallback;
    if ((text[0] == L'F' || text[0] == L'f') && text.size() > 1) {
        const int n = _wtoi(text.c_str() + 1);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : fallback;
    }
    wchar_t* end = nullptr;
    const long value = std::wcstol(text.c_str(), &end, 0);
    return end != text.c_str() && value > 0 && value < 256 ? static_cast<int>(value) : fallback;
}

void Write(const wchar_t* section, const wchar_t* key, const std::string& value) {
    if (g_file.empty()) return;
    const std::wstring wide(value.begin(), value.end());
    WritePrivateProfileStringW(section, key, wide.c_str(), g_file.c_str());
}

}  // namespace

void Load(const std::filesystem::path& file) {
    Config c;
    g_file = file;
    if (!std::filesystem::exists(file)) {
        log::Warn("Config '{}' not found, using defaults", file.string());
        g_config = c;
        return;
    }

    c.enabled = ReadBool(L"General", L"Enabled", c.enabled);
    c.logLevel = log::ParseLevel(Narrow(ReadString(L"General", L"LogLevel", L"")), c.logLevel);

    c.chainLoadDxgi = ReadString(L"Loader", L"ChainLoadDXGI", L"");
    c.allowUnsignedDlls = ReadBool(L"Loader", L"AllowUnsignedDlls", c.allowUnsignedDlls);

    c.upscalerEnabled = ReadBool(L"Upscaler", L"Enabled", c.upscalerEnabled);
    c.backend = ParseBackend(Narrow(ReadString(L"Upscaler", L"Backend", L"")), c.backend);
    c.quality = ParseQualityMode(Narrow(ReadString(L"Upscaler", L"Quality", L"")), c.quality);
    c.dlssPreset = ParseDlssPreset(Narrow(ReadString(L"Upscaler", L"DlssPreset", L"")), c.dlssPreset);
    c.fsrVersion = Narrow(ReadString(L"Upscaler", L"FsrVersion", L""));
    c.jitterFlipX = ReadBool(L"Upscaler", L"JitterFlipX", c.jitterFlipX);
    c.jitterFlipY = ReadBool(L"Upscaler", L"JitterFlipY", c.jitterFlipY);
    c.fillScreen = ReadBool(L"Upscaler", L"FillScreen", c.fillScreen);

    c.dlss5Enabled = ReadBool(L"DLSS5", L"Enabled", c.dlss5Enabled);
    c.dlss5Key = ReadKey(L"DLSS5", L"Key", c.dlss5Key);
    c.dlss5Intensity = std::clamp(ReadFloat(L"DLSS5", L"Intensity", c.dlss5Intensity), 0.0f, 2.0f);
    c.dlss5LocalTone = ReadFloat(L"DLSS5", L"LocalTone", c.dlss5LocalTone);
    c.dlss5LocalStructure = ReadFloat(L"DLSS5", L"LocalStructure", c.dlss5LocalStructure);
    c.dlss5SkinStructure = ReadFloat(L"DLSS5", L"SkinStructure", c.dlss5SkinStructure);
    c.dlss5Style = std::clamp(ReadInt(L"DLSS5", L"Style", c.dlss5Style), 0, 7);

    c.frameGenerationBackend =
        ParseBackend(Narrow(ReadString(L"FrameGeneration", L"Backend", L"")), c.frameGenerationBackend);
    c.frameGeneration = ReadBool(L"FrameGeneration", L"Enabled", c.frameGeneration);
    c.generatedFrames = std::clamp(ReadInt(L"FrameGeneration", L"GeneratedFrames", c.generatedFrames), 1, 3);
    c.presenter = ReadBool(L"FrameGeneration", L"Presenter", c.presenter);

    c.vsync = std::clamp(ReadInt(L"Game", L"VSync", c.vsync), -1, 1);
    c.targetFps = std::clamp(ReadInt(L"Game", L"TargetFPS", c.targetFps), 60, 500);

    c.overlayKey = ReadKey(L"Overlay", L"MenuKey", c.overlayKey);
    c.overlayStatusLine = ReadBool(L"Overlay", L"StatusLine", c.overlayStatusLine);
    c.upscalerKey = ReadKey(L"Overlay", L"UpscalerKey", c.upscalerKey);

    c.logResources = ReadBool(L"Debug", L"LogResources", c.logResources);
    g_config = c;
}

const Config& Get() { return g_config; }

void SaveUpscaler(bool enabled, Backend backend, QualityMode quality) {
    g_config.upscalerEnabled = enabled;
    g_config.backend = backend;
    g_config.quality = quality;
    Write(L"Upscaler", L"Enabled", enabled ? "1" : "0");
    Write(L"Upscaler", L"Backend", ToString(backend));
    Write(L"Upscaler", L"Quality", ToString(quality));
}

void SaveDlssPreset(uint32_t preset) {
    g_config.dlssPreset = preset;
    Write(L"Upscaler", L"DlssPreset", DlssPresetName(preset));
}

void SaveFsrVersion(const std::string& version) {
    g_config.fsrVersion = version;
    Write(L"Upscaler", L"FsrVersion", version);
}

void SaveJitter(bool flipX, bool flipY) {
    g_config.jitterFlipX = flipX;
    g_config.jitterFlipY = flipY;
    Write(L"Upscaler", L"JitterFlipX", flipX ? "1" : "0");
    Write(L"Upscaler", L"JitterFlipY", flipY ? "1" : "0");
}

void SaveFrameGeneration(bool enabled, int generatedFrames) {
    g_config.frameGeneration = enabled;
    g_config.generatedFrames = generatedFrames;
    Write(L"FrameGeneration", L"Enabled", enabled ? "1" : "0");
    Write(L"FrameGeneration", L"GeneratedFrames", std::to_string(generatedFrames));
}

void SaveFrameGenerationBackend(Backend backend) {
    // Not applied to g_config: the session keeps the backend it started with.
    Write(L"FrameGeneration", L"Backend", ToString(backend));
}


void SaveOverlayStatusLine(bool visible) {
    g_config.overlayStatusLine = visible;
    Write(L"Overlay", L"StatusLine", visible ? "1" : "0");
}

void SaveDlss5(bool enabled, float intensity, int style) {
    g_config.dlss5Enabled = enabled;
    g_config.dlss5Intensity = intensity;
    g_config.dlss5Style = style;
    Write(L"DLSS5", L"Enabled", enabled ? "1" : "0");
    Write(L"DLSS5", L"Intensity", std::format("{:.2f}", intensity));
    Write(L"DLSS5", L"Style", std::to_string(style));
}

void SaveTargetFps(int fps) {
    g_config.targetFps = fps;
    Write(L"Game", L"TargetFPS", std::to_string(fps));
}

}  // namespace dsru::config
