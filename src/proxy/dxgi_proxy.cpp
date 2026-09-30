// dxgi.dll proxy. DarkSoulsRemastered.exe imports dxgi.dll (CreateDXGIFactory), so this DLL is loaded
// from the game folder instead of System32; d3d11.dll binds its own dxgi imports to it too (a module
// named dxgi.dll is already loaded). Every export forwards to the real dxgi.dll, or to a chain-loaded
// one (ReShade renamed to ReShade64.dll), and lets the host DLL see each factory.

#include "core/log.h"
#include "core/paths.h"
#include "proxy/proxy_host.h"

#include <Windows.h>
#include <dxgi1_6.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace {

HMODULE g_realDxgi = nullptr;
std::once_flag g_loadOnce;

void LoadRealDxgi() {
    using namespace dsru;

    // The system dxgi.dll is loaded first even when chain-loading: a hooking injector (ReShade loaded
    // as ReShade64.dll) hooks the system dxgi's exports when it initializes and forwards its own
    // exports there; with the system dxgi not loaded yet, its hooks stay pending.
    wchar_t systemDir[MAX_PATH];
    GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::filesystem::path systemPath = std::filesystem::path(systemDir) / L"dxgi.dll";
    const HMODULE systemDxgi = LoadLibraryW(systemPath.c_str());
    if (!systemDxgi) log::Error("Failed to load system dxgi.dll (error {})", GetLastError());

    const std::wstring chain = proxy::ChainLoadPath();
    if (!chain.empty()) {
        std::filesystem::path path = chain;
        if (path.is_relative()) path = paths::ModuleDirectory() / path;
        g_realDxgi = LoadLibraryW(path.c_str());
        if (g_realDxgi)
            log::Info("Chain-loaded dxgi: {}", path.string());
        else
            log::Error("Failed to chain-load '{}' (error {}), falling back to system dxgi", path.string(),
                       GetLastError());
    }
    if (!g_realDxgi) g_realDxgi = systemDxgi;
}

template <class Fn>
Fn ResolveReal(const char* name) {
    dsru::proxy::EnsureInitialized();
    std::call_once(g_loadOnce, LoadRealDxgi);
    auto fn = g_realDxgi ? reinterpret_cast<Fn>(GetProcAddress(g_realDxgi, name)) : nullptr;
    if (!fn) dsru::log::Warn("Real dxgi has no export '{}'", name);
    return fn;
}

HRESULT AfterFactoryCreated(HRESULT hr, void** factory) {
    if (SUCCEEDED(hr) && factory && *factory) dsru::proxy::OnFactoryCreated(static_cast<IUnknown*>(*factory));
    return hr;
}

using CreateDXGIFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
using DXGIGetDebugInterface1_t = HRESULT(WINAPI*)(UINT, REFIID, void**);
using VoidHResult_t = HRESULT(WINAPI*)();

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_CreateDXGIFactory(REFIID riid, void** factory) {
    static auto real = ResolveReal<CreateDXGIFactory_t>("CreateDXGIFactory");
    if (!real) return E_NOTIMPL;
    return AfterFactoryCreated(real(riid, factory), factory);
}

HRESULT WINAPI Proxy_CreateDXGIFactory1(REFIID riid, void** factory) {
    static auto real = ResolveReal<CreateDXGIFactory_t>("CreateDXGIFactory1");
    if (!real) return E_NOTIMPL;
    return AfterFactoryCreated(real(riid, factory), factory);
}

HRESULT WINAPI Proxy_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    static auto real = ResolveReal<CreateDXGIFactory2_t>("CreateDXGIFactory2");
    if (!real) return E_NOTIMPL;
    return AfterFactoryCreated(real(flags, riid, factory), factory);
}

// Not exported under this name (the .def maps the exports to Proxy_*): code linked into this DLL
// that calls these directly (our D3D12 bridge, Dear ImGui) gets the real factory, since we can't
// link dxgi.lib - we are dxgi.dll.
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) { return Proxy_CreateDXGIFactory1(riid, factory); }
HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    return Proxy_CreateDXGIFactory2(flags, riid, factory);
}

HRESULT WINAPI Proxy_DXGIDeclareAdapterRemovalSupport() {
    static auto real = ResolveReal<VoidHResult_t>("DXGIDeclareAdapterRemovalSupport");
    return real ? real() : E_NOTIMPL;
}

HRESULT WINAPI Proxy_DXGIDisableVBlankVirtualization() {
    static auto real = ResolveReal<VoidHResult_t>("DXGIDisableVBlankVirtualization");
    return real ? real() : E_NOTIMPL;
}

HRESULT WINAPI Proxy_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug) {
    static auto real = ResolveReal<DXGIGetDebugInterface1_t>("DXGIGetDebugInterface1");
    return real ? real(flags, riid, debug) : E_NOTIMPL;
}

}  // extern "C"
