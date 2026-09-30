#include "game/dshr.h"

#include "core/log.h"
#include "core/paths.h"

#include <Windows.h>

#include <filesystem>

namespace dsru::game::dshr {

Status Get() {
    Status s;
    const auto dir = paths::ExecutablePath().parent_path();
    const auto ini = dir / "DSHRHook.ini";
    s.installed = std::filesystem::exists(dir / "dinput8.dll") && std::filesystem::exists(ini);
    if (HMODULE module = GetModuleHandleW(L"dinput8.dll")) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        s.loaded = _wcsicmp(std::filesystem::path(path).parent_path().c_str(), dir.c_str()) == 0;
    }
    if (s.installed) s.targetFps = static_cast<int>(GetPrivateProfileIntW(L"DSHR", L"TargetFPS", 0, ini.c_str()));
    return s;
}

void LogStatus() {
    const Status s = Get();
    if (s.installed)
        log::Info("DSHR: installed (TargetFPS={}) - the frame rate is its business", s.targetFps);
    else
        log::Info("DSHR: not installed - the game keeps its 60 FPS limit (DSHR unlocks it with the game's speed kept)");
}

}  // namespace dsru::game::dshr
