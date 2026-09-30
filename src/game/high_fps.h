#pragma once

// Frame rate unlock with the game's speed kept. The engine is built around 60 FPS: its frame limiter
// runs a clock at 60 ticks per second, and the update code hands a fixed 1/60 s time step to the
// subsystems whatever the real frame time. Above 60 FPS the game would run too fast, so the unlock
// raises the limiter and replaces that step with the last frame's real duration, at most 1/60 s: the
// speed is right at any frame rate from 60 up to the target (below 60 the game slows down, as the
// original does; longer steps than the game ever had would risk its physics):
//  - pacing: the limiter clock (a global, the object game/frame_timer.h describes) gets the target
//    rate, its start moved so the schedule keeps its phase. The flipper's high refresh mode (owner mode
//    5, FrpgFlipper_140FPS) is left alone: the engine sets its mode back every frame, and the clock
//    alone sets the frame rate.
//  - simulation: MoveMapStep's step table (.data, the step at index 13 is the in-game update, float
//    time step in xmm1), the cutscene (Remo) step table's index 9, the FX manager (frpgFxManagerBase,
//    vtable slot 5) and the Havok world step (a call site in the main update function, relayed).
//  - input: the repeat detectors (DLUI::DLUserInputRepeatDetector, slots 4 / 5) count their delays
//    down by the time step; the menus act on a button press for as many frames as the press lasts, so
//    one press is let through once per menu (the "menu action query" function, hooked); mouse deltas
//    (DirectInput) are divided by the step scale as the camera turns by delta x time step.
// Everything is found at runtime (patterns, RTTI); the code patches (Havok call site, menu query) are
// made once the game has run for a few seconds, as the executable's protection only checks at startup.
namespace dsru::game::high_fps {

inline constexpr int kGameFps = 60;
inline constexpr int kMaxFps = 500;

// Finds the engine's pieces and installs the data hooks (bootstrap, after hooks::Initialize).
// `targetFps` <= 60: the game's own 60 FPS until SetTarget.
void Install(int targetFps);

// Every frame, on the thread that presents (the engine's frame limiter runs there).
void OnFrame();

// 60 = the game's 60 FPS; above, the unlock (applies at the next frame).
void SetTarget(int fps);

struct Status {
    bool available = false;  // everything found and hooked
    bool active = false;     // the engine runs at `target`
    int target = kGameFps;
    const char* reason = "";  // why unavailable or not active yet
};
Status GetStatus();

}  // namespace dsru::game::high_fps
