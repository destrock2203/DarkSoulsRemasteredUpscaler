#include "core/paths.h"

#include <string>

namespace dsru::paths {

namespace {

HMODULE g_self = nullptr;

std::filesystem::path ModuleFileName(HMODULE module) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) return {};
        if (len < buffer.size()) {
            buffer.resize(len);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace

void SetSelfModule(HMODULE module) { g_self = module; }

HMODULE SelfModule() { return g_self; }

std::filesystem::path ModuleDirectory() { return ModuleFileName(g_self).parent_path(); }

std::filesystem::path ExecutablePath() { return ModuleFileName(nullptr); }

}  // namespace dsru::paths
