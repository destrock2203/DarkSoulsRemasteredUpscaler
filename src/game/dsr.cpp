#include "game/dsr.h"

#include "core/log.h"
#include "core/paths.h"

#include <Windows.h>

#include <format>
#include <string>
#include <vector>

namespace dsru::game {

namespace {

std::string FileVersion(const std::filesystem::path& file) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (!size) return "unknown";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return "unknown";
    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

}  // namespace

void LogGameInfo() {
    const auto exe = paths::ExecutablePath();
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    log::Info("Host: {} (version {}, PE timestamp 0x{:08x})", exe.filename().string(), FileVersion(exe),
              nt->FileHeader.TimeDateStamp);
    if (_wcsicmp(exe.filename().c_str(), L"DarkSoulsRemastered.exe") != 0)
        log::Warn("Host is not DarkSoulsRemastered.exe - the game-specific parts will not find what they expect");
}

}  // namespace dsru::game
