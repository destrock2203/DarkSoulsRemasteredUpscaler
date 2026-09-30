// DSRProbe entry points: DllMain, one-time initialization, settings, the dxgi proxy host.

#include "core/crash_handler.h"
#include "core/log.h"
#include "core/paths.h"
#include "game/flipper.h"
#include "game/shader_db.h"
#include "hooks/hook_manager.h"
#include "probe/probe.h"
#include "proxy/proxy_host.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>

#include <cwchar>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace dsru::probe {

namespace {

std::once_flag g_initOnce;
Settings g_settings;

int ReadKey(const std::filesystem::path& file, const wchar_t* key, int fallback) {
    wchar_t buffer[64];
    GetPrivateProfileStringW(L"Probe", key, L"", buffer, 64, file.c_str());
    const std::wstring text = buffer;
    if (text.empty()) return fallback;
    if ((text[0] == L'F' || text[0] == L'f') && text.size() > 1) {
        const int n = _wtoi(text.c_str() + 1);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : fallback;
    }
    wchar_t* end = nullptr;
    const long value = std::wcstol(text.c_str(), &end, 0);
    return end != text.c_str() && value > 0 && value < 256 ? static_cast<int>(value) : fallback;
}

void LoadSettings(const std::filesystem::path& file) {
    Settings s;
    wchar_t buffer[1024];
    GetPrivateProfileStringW(L"Probe", L"ChainLoadDXGI", L"", buffer, 1024, file.c_str());
    s.chainLoadDxgi = buffer;
    s.traceKey = ReadKey(file, L"TraceKey", s.traceKey);
    s.flipperKey = ReadKey(file, L"FlipperKey", s.flipperKey);
    s.snapshotKey = ReadKey(file, L"SnapshotKey", s.snapshotKey);
    s.traceFrames = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Probe", L"TraceFrames", s.traceFrames, file.c_str())), 1, 4);
    s.traceAtFrame = static_cast<int>(GetPrivateProfileIntW(L"Probe", L"TraceAtFrame", 0, file.c_str()));
    s.dumpTextures = GetPrivateProfileIntW(L"Probe", L"DumpTextures", 1, file.c_str()) != 0;
    s.maxDumps = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Probe", L"MaxDumps", s.maxDumps, file.c_str())), 0, 256);
    s.dumpUnknownShaders = GetPrivateProfileIntW(L"Probe", L"DumpUnknownShaders", 1, file.c_str()) != 0;
    s.logResources = GetPrivateProfileIntW(L"Probe", L"LogResources", 1, file.c_str()) != 0;
    g_settings = s;
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

std::string FileVersion(const std::filesystem::path& file) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (!size) return "unknown";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return "unknown";
    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

void LogEnvironment() {
    const auto exe = paths::ExecutablePath();
    std::error_code ec;
    const auto size = std::filesystem::file_size(exe, ec);
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    log::Info("Host: {} version {} ({} bytes, PE timestamp 0x{:08x}, image size 0x{:x})", Narrow(exe.wstring()),
              FileVersion(exe), size, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    if (nt->FileHeader.TimeDateStamp != 0x6344ca56)
        log::Warn("This executable is not the build the probe was written against (0x6344ca56, Oct 2022)");

    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto fn = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
        OSVERSIONINFOW v{sizeof(v)};
        if (fn(&v) == 0) log::Info("Windows {}.{} build {}", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
        log::Info("Primary display: {}x{} @ {} Hz", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
    log::Info("Shader table: {} ids from the game's archives", game::shaders::Count());

    // Modules already loaded (overlays, injectors).
    HMODULE modules[512];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        std::string list;
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i) {
            wchar_t name[MAX_PATH];
            if (GetModuleFileNameW(modules[i], name, MAX_PATH)) {
                const wchar_t* file = wcsrchr(name, L'\\');
                list += (list.empty() ? "" : ", ") + Narrow(file ? file + 1 : name);
            }
        }
        log::Info("Loaded modules: {}", list);
    }
}

void Initialize() {
    const auto dir = paths::ModuleDirectory();
    std::error_code ec;
    std::filesystem::copy_file(dir / "DSRProbe.log", dir / "DSRProbe.prev.log",
                               std::filesystem::copy_options::overwrite_existing, ec);
    log::Init(dir / "DSRProbe.log");
    log::SetLevel(log::Level::Debug);
    LoadSettings(dir / "DSRProbe.ini");
    crash::Install();
    log::Info("DSRProbe {} - diagnostics only, the game is not modified", DSRU_VERSION);
    log::Info("Keys: F10 (TraceKey) frame trace, F11 (FlipperKey) flipper 60/140 FPS test, F9 (SnapshotKey) data "
              "snapshot. Output: {}", Narrow(OutputDirectory().wstring()));
    LogEnvironment();
    if (!hooks::Initialize()) {
        log::Error("MinHook failed to initialize - probe inactive");
        return;
    }
    InstallTimingHooks();
    if (game::flipper::Initialize()) log::Info("Flipper classes: {}", game::flipper::Describe());
}

}  // namespace

const Settings& Get() { return g_settings; }

std::filesystem::path OutputDirectory() { return paths::ModuleDirectory(); }

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R16G16_TYPELESS: return "R16G16_TYPELESS";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
        case DXGI_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return "X24_TYPELESS_G8_UINT";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8G8_TYPELESS: return "R8G8_TYPELESS";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
        case DXGI_FORMAT_R8_TYPELESS: return "R8_TYPELESS";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_A8_UNORM: return "A8_UNORM";
        case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC2_UNORM: return "BC2_UNORM";
        case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC4_UNORM: return "BC4_UNORM";
        case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
        case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
        default: {
            static thread_local char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "FMT_%d", static_cast<int>(f));
            return buffer;
        }
    }
}

std::string BindFlagsName(UINT flags) {
    std::string s;
    auto add = [&](UINT bit, const char* name) {
        if (flags & bit) s += (s.empty() ? "" : "|") + std::string(name);
    };
    add(D3D11_BIND_VERTEX_BUFFER, "VB");
    add(D3D11_BIND_INDEX_BUFFER, "IB");
    add(D3D11_BIND_CONSTANT_BUFFER, "CB");
    add(D3D11_BIND_SHADER_RESOURCE, "SRV");
    add(D3D11_BIND_STREAM_OUTPUT, "SO");
    add(D3D11_BIND_RENDER_TARGET, "RT");
    add(D3D11_BIND_DEPTH_STENCIL, "DS");
    add(D3D11_BIND_UNORDERED_ACCESS, "UAV");
    return s.empty() ? "-" : s;
}

std::string DescribeAddress(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module))
        return std::format("0x{:x}", reinterpret_cast<uintptr_t>(address));
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return std::format("{}+0x{:x}", Narrow(name ? name + 1 : path),
                       reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
}

}  // namespace dsru::probe

namespace dsru::proxy {

void EnsureInitialized() { std::call_once(probe::g_initOnce, probe::Initialize); }

std::wstring ChainLoadPath() { return probe::Get().chainLoadDxgi; }

void OnFactoryCreated(IUnknown* factory) { probe::OnFactoryCreated(factory); }

}  // namespace dsru::proxy

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            dsru::paths::SetSelfModule(module);
            // Import-table patch only: safe under the loader lock, and in place before the game
            // creates its device.
            dsru::probe::InstallEarlyHooks();
            break;
        case DLL_PROCESS_DETACH:
            if (!reserved) dsru::hooks::Shutdown();
            dsru::log::Info("Process exiting");
            dsru::log::Shutdown();
            break;
    }
    return TRUE;
}
