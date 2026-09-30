#pragma once

#include <cstdint>

// The engine's frame limiter (found with DSRProbe: a QueryPerformanceCounter busy-wait on the present
// thread, ~500 000 calls per frame). A timer object emulates a V-Sync clock:
//     exe+0xce3450  WaitUntil(timer, tick):  while ((QPC - timer+0x678) * timer+0x60 / frequency < tick) spin
//                   (returns at once when timer+0x60 is 0)
//     caller        WaitUntil(timer, timer+0x690); timer+0x688 = QPC; ++timer+0x690
// timer+0x60 is the clock's rate: 60 = 60 FPS. The object is found at runtime without touching code:
// the executable's QueryPerformanceCounter import is hooked, the value returned to the call right
// after the wait (the one stored to timer+0x688) is kept, and the heap is scanned for it
// (timer+0x60 == 60 confirms). The rate is then a plain data write: 0 = no limit.
namespace dsru::game::frame_timer {

// Finds the call site (pattern) and hooks the executable's QueryPerformanceCounter import. Safe to
// call from DllMain (import-table patch). The probe, which hooks the same import, calls OnQpc from
// its own hook instead (`hookImport` false).
void Install(bool hookImport);

// From a QueryPerformanceCounter hook: the caller's return address and the value returned.
void OnQpc(const void* returnAddress, int64_t value);

// Present thread, once per frame: starts the background scan once the call site has run.
void OnPresent();

bool Found();
// The clock's rate (FPS), -1 before the object is found.
int Rate();
// 0 = no limit. False before the object is found or if it is gone.
bool SetRate(int fps);

}  // namespace dsru::game::frame_timer
