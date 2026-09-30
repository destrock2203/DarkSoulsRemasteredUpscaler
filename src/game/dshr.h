#pragma once

// DSHR (DSHRHook, a dinput8.dll mod) unlocks Dark Souls Remastered's frame rate properly: it switches
// the engine's frame pacing to its high refresh mode and corrects the game's time steps (simulation,
// Havok physics, effects, input, cutscenes) so the game keeps its speed above 60 FPS. The mod leaves the
// frame rate to it: both load side by side (dinput8.dll and dxgi.dll). The overlay shows its state.
namespace dsru::game::dshr {

struct Status {
    bool installed = false;  // dinput8.dll + DSHRHook.ini next to the game
    bool loaded = false;     // its dinput8.dll is loaded in the game
    int targetFps = 0;       // DSHRHook.ini [DSHR] TargetFPS
};
Status Get();

// Logs what was found (startup).
void LogStatus();

}  // namespace dsru::game::dshr
