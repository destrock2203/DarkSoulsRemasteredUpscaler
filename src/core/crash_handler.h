#pragma once

// Logs fatal exceptions (access violations, illegal instructions, C++ throws, ...) with the
// faulting module+offset and a stack walk, so a crash leaves a trail in the log. Offsets inside
// dxgi.dll can be resolved with build/<preset>/dxgi.map. The handler only observes: it always
// lets the exception continue to the game's own handlers.
namespace dsru::crash {

void Install();

// While alive, access violations on this thread are not reported: for code that deliberately
// reads memory that may be gone (under SEH) and handles the fault itself.
class ExpectFaults {
public:
    ExpectFaults();
    ~ExpectFaults();
    ExpectFaults(const ExpectFaults&) = delete;
    ExpectFaults& operator=(const ExpectFaults&) = delete;
};

}  // namespace dsru::crash
