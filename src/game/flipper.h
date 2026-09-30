#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The engine's frame pacing object ("flipper"). NS_FRPG::FrpgFlipper has one subclass per pacing
// mode, found through RTTI (game/rtti.h); the debug menu names them "FlipMode":
//     FrpgFlipper_30FPS, _30FPS_DS1, _30FPS_DS1_PAL, _30FPS_NoSkip   (Prepare To Die Edition modes)
//     FrpgFlipper_60FPS (the PC default), _60FPS_NoSkip
//     FrpgFlipper_140FPS                                              (developer mode)
// Their virtual functions are tiny constant getters (vtable slot 8 writes two smoothing factors:
// 60 FPS 11/12 and 0.95, 30 FPS half of that, 140 FPS 55/56 and 139/140). The engine asks the live
// flipper for its pacing each frame, so changing the object's vtable pointer to another subclass's
// switches the pacing mode without patching code (the executable is protected against code
// changes). The live object is found by scanning the process heap for a flipper vtable pointer.
namespace dsru::game::flipper {

enum class Mode : uint8_t { k30, k30Ds1, k30Ds1Pal, k30NoSkip, k60, k60NoSkip, k140, kCount };

const char* Name(Mode mode);  // "60FPS", ...

// Finds the vtables of every mode (once). False if the base class isn't in this executable.
bool Initialize();

struct Instance {
    uintptr_t address = 0;
    Mode mode = Mode::kCount;  // kCount: an object of the base class FrpgFlipper
};

// Scans committed, writable private memory for objects whose first qword is a flipper vtable.
// Slow (the whole heap: a second or more): call from a background thread.
std::vector<Instance> Scan();

// The instance found by the last Scan that the engine uses (the first one), if any.
bool Current(Instance& out);

// Switches the current instance to `mode` (vtable pointer swap). False without an instance.
bool SetMode(Mode mode);

// Readable summary of the vtables found and the current instance, for logs.
std::string Describe();

}  // namespace dsru::game::flipper
