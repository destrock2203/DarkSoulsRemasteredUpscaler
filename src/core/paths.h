#pragma once

#include <Windows.h>

#include <filesystem>

namespace dsru::paths {

void SetSelfModule(HMODULE module);
HMODULE SelfModule();

// Folder containing our dxgi.dll (normally DARK SOULS REMASTERED).
std::filesystem::path ModuleDirectory();

// Full path of the host executable.
std::filesystem::path ExecutablePath();

}  // namespace dsru::paths
