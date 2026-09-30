#pragma once

// Frame timing split, measured in the Present hook (main thread):
//   game  = from the previous Present returning to this Present being called (game CPU work,
//           including its frame limiter sleep)
//   mod   = our own work inside the Present hook (overlay, DLSS bookkeeping)
//   wait  = time blocked in the native Present (GPU behind / V-Sync)
// A GPU-bound game spends its time in `wait`; a CPU-bound one in `game`.
namespace dsru::perf {

void PresentEntered();
void NativePresentCalled();
void PresentReturned();

struct Stats {
    float frameMs = 0;
    float gameMs = 0;
    float modMs = 0;
    float waitMs = 0;
};
Stats Get();

}  // namespace dsru::perf
