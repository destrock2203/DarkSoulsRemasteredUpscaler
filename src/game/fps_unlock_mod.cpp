#include "game/fps_unlock_mod.h"

#include "core/log.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace dsru::game::fps_unlock_mod {

namespace {

Info g_info;

std::string Trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r") + 1 - begin);
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool ParseBool(const std::string& value) {
    const std::string v = Lower(value);
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

// Its ini as it parses it: `key = value` lines, '#' starts a comment, sections ignored, keys in any case.
// Defaults: its own (FPSUnlock on, MaxFPS 120 clamped to 10 - 1000, VariableFrameTime on).
void ReadIni(const std::filesystem::path& file, bool& fpsUnlock) {
    g_info.maxFps = 120;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        if (key == "fpsunlock") {
            fpsUnlock = ParseBool(value);
        } else if (key == "maxfps" || key == "targetfps") {
            char* end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            if (end != value.c_str()) g_info.maxFps = static_cast<int>(std::clamp(n, 10L, 1000L));
        } else if (key == "variableframetime") {
            g_info.variableFrameTime = ParseBool(value);
        }
    }
}

// Its log starts with "DS1 Remastered FPS Unlock v<version>".
std::string ReadVersion(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    for (int i = 0; i < 20 && std::getline(in, line); ++i)
        if (const auto at = line.find("FPS Unlock v"); at != std::string::npos)
            return Trim(line.substr(at + 12, line.find_first_of(" \r", at + 12) - (at + 12)));
    return {};
}

}  // namespace

const Info& Detect() {
    g_info = {};
    HMODULE module = GetModuleHandleW(L"dinput8.dll");
    wchar_t path[MAX_PATH];
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) return g_info;
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    const std::filesystem::path ini = dir / "DSR-FPS-Unlock.ini";
    std::error_code ec;
    if (!std::filesystem::exists(ini, ec)) return g_info;  // the system's dinput8.dll, or another mod's
    bool fpsUnlock = true;
    ReadIni(ini, fpsUnlock);
    g_info.version = ReadVersion(dir / "DSR-FPS-Unlock.log");
    if (!fpsUnlock) {
        log::Info("DS1 FPS Unlock {}: FPSUnlock = false in its ini - the game keeps its 60 FPS", g_info.version);
        g_info.maxFps = 0;
        return g_info;
    }
    g_info.loaded = true;
    log::Info("DS1 FPS Unlock {} loaded: MaxFPS {}, {}", g_info.version.empty() ? "?" : g_info.version, g_info.maxFps,
              g_info.variableFrameTime ? "measured frame time" : "fixed 1/MaxFPS steps");
    if (!g_info.variableFrameTime)
        log::Warn("DS1 FPS Unlock: VariableFrameTime = false - the game's speed follows MaxFPS, a limit set in the "
                  "menu would change it");
    return g_info;
}

const Info& Get() { return g_info; }

}  // namespace dsru::game::fps_unlock_mod
