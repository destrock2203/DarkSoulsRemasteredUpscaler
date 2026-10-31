#pragma once

#include <string>

// DS1 FPS Unlock (zofrea1, MIT, https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered), which the
// player installs next to the game: dinput8.dll (the game imports it) and DSR-FPS-Unlock.ini. It unlocks
// the frame rate with the game's speed kept; when it is loaded, the menu sets its limit live
// (game/high_fps.h). It reads its ini once at startup.
namespace dsru::game::fps_unlock_mod {

struct Info {
    bool loaded = false;            // its dinput8.dll is loaded, with FPSUnlock on
    int maxFps = 0;                 // its MaxFPS, as it read it at startup
    bool variableFrameTime = true;  // false: fixed 1/MaxFPS steps, the game's speed follows MaxFPS
    std::string version;            // from its log, empty if not found
};

// Looks for it (bootstrap, before high_fps::Install).
const Info& Detect();
const Info& Get();

}  // namespace dsru::game::fps_unlock_mod
