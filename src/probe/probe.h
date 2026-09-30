#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <filesystem>
#include <string>

// DSRProbe: a diagnostic dxgi.dll for Dark Souls Remastered. It changes nothing in the game; it logs
// what the mod needs to know (DSRProbe.log) and writes frame traces on demand:
//  - startup: executable, modules, GPU / driver, display, the D3D11 device and swapchain
//  - render: every texture / view / shader the game creates (shaders named from the game's own
//    archives, game/shader_db.h), the post-processing passes each frame runs (logged whenever they
//    change: toggle a graphics option and see which passes appear), and on F10 a full trace of two
//    frames with every draw, its shaders, render targets, textures and constants (DSRProbe_trace_*.txt)
//    plus PNG dumps of the key render targets (DSRProbe_dumps/)
//  - timing: who sleeps / waits between frames (the frame limiter), present intervals, V-Sync
//  - memory: the engine's frame pacing object (game/flipper.h), F11 switches it to 140 FPS as a test;
//    F9 snapshots the executable's data to find a setting (snapshot, change the setting, snapshot,
//    change it back, snapshot: addresses that followed are logged)
namespace dsru::probe {

struct Settings {
    std::wstring chainLoadDxgi;
    int traceKey = VK_F10;
    int traceFrames = 2;
    int traceAtFrame = 0;  // also trace automatically at this present (0 = key only)
    bool dumpTextures = true;
    int maxDumps = 48;
    int flipperKey = VK_F11;
    int snapshotKey = VK_F9;
    bool dumpUnknownShaders = true;  // write shaders missing from the table to DSRProbe_shaders/
    bool logResources = true;        // every render target / depth / UAV texture created
};

const Settings& Get();
std::filesystem::path OutputDirectory();

// --- d3d11 (probe_d3d11.cpp) ---
// From DllMain: import-table hook of d3d11!D3D11CreateDevice in the executable.
void InstallEarlyHooks();
void OnFactoryCreated(IUnknown* factory);
// The game's present thread, before the native Present: frame accounting, trace control.
void OnPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
// Presents counted so far.
uint64_t PresentCount();
// True while a frame trace is recording (timing and memory tools add their events to it).
bool Tracing();
// A line for the trace file (other probe modules), in present order.
void TraceNote(std::string text);

// --- timing (probe_timing.cpp) ---
void InstallTimingHooks();
void TimingOnPresent(uint64_t present);

// --- memory (probe_memory.cpp) ---
void MemoryOnPresent(uint64_t present, bool flipperKey, bool snapshotKey);

// --- texture dumps (probe_dump.cpp) ---
namespace dump {
// Records a copy of `resource` (subresource 0) into a staging texture on `context` (deferred or
// immediate) after a draw; the pixels are read back after the frame. `label` names the PNG.
void Capture(ID3D11DeviceContext* context, ID3D11Resource* resource, std::string label);
// After the traced frames' last Present (immediate context, present thread): reads every capture
// back and writes the PNGs from a worker thread.
void Flush(ID3D11DeviceContext* immediate, const std::filesystem::path& directory);
int Pending();
}  // namespace dump

// Readable names.
const char* FormatName(DXGI_FORMAT format);
std::string BindFlagsName(UINT flags);
// "DarkSoulsRemastered.exe+0x1234" for a code address.
std::string DescribeAddress(const void* address);

}  // namespace dsru::probe
