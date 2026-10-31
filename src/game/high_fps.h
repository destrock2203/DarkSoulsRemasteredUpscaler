#pragma once

// Live frame rate limit, with DS1 FPS Unlock (game/fps_unlock_mod.h) loaded: that mod keeps the game's
// speed at any frame rate (time steps from the measured frame time, physics, input and menu fixes) and
// paces it with the engine's frame limiter; the limiter's clock is set here, so the menu moves the
// limit while the game runs. Without the mod the game keeps its own 60 FPS: nothing is touched.
//
// The engine's frame limiter waits each frame until (QPC - start) x rate / frequency reaches the frame
// count, on a clock object (a global, the object game/frame_timer.h describes) at 60 ticks per second.
// The mod hands that wait a time running MaxFPS/60 times faster than real time, so the rate set here
// is the target x 60 / MaxFPS, the clock's start moved so the schedule keeps its phase (the mod follows
// a moved start). Found at runtime (pattern).
namespace dsru::game::high_fps {

inline constexpr int kGameFps = 60;
inline constexpr int kMinFps = 30;
inline constexpr int kMaxFps = 500;

// Finds the engine's frame clock (bootstrap). `modFps`: DS1 FPS Unlock's MaxFPS, 0 = not loaded (then
// nothing happens). `targetFps`: the limit, 0 = the mod's MaxFPS until SetTarget.
void Install(int targetFps, int modFps);

// Every frame, on the thread that presents (the engine's frame limiter runs there).
void OnFrame();

// The limit, applied at the next frame.
void SetTarget(int fps);

struct Status {
    bool available = false;  // the mod is loaded and the clock was found
    bool active = false;     // the clock runs at `target`
    int target = kGameFps;
    int modFps = 0;           // the mod's MaxFPS (0: not loaded)
    const char* reason = "";  // why unavailable or not active yet
};
Status GetStatus();

}  // namespace dsru::game::high_fps
