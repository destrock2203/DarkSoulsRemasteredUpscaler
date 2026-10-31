// DSRUpscaler entry points: one-time initialization (from the first dxgi export call or the game's
// D3D11CreateDevice, never under the loader lock), the dxgi proxy host, process exit.

#include "core/config.h"
#include "core/crash_handler.h"
#include "core/log.h"
#include "core/paths.h"
#include "game/dsr.h"
#include "game/fps_unlock_mod.h"
#include "game/high_fps.h"
#include "hooks/dxgi_hooks.h"
#include "hooks/hook_manager.h"
#include "proxy/proxy_host.h"
#include "render/bridge.h"
#include "render/upscale.h"
#include "upscaler/streamline/streamline.h"

#include <Windows.h>

#include <filesystem>
#include <mutex>

namespace dsru::core {

namespace {

std::once_flag g_initOnce;

// The game quits through ExitProcess, which unloads DLLs in an order Streamline / NGX don't survive
// unless Streamline was shut down first.
using RtlExitUserProcess_t = void(NTAPI*)(LONG);
RtlExitUserProcess_t oRtlExitUserProcess = nullptr;

void NTAPI hkRtlExitUserProcess(LONG status) {
    log::Info("Process exiting (status 0x{:x}): shutting the upscaler down", static_cast<unsigned long>(status));
    render::upscale::Shutdown();
    render::bridge::Shutdown();
    streamline::Shutdown();
    oRtlExitUserProcess(status);
}

void Initialize() {
    const auto dir = paths::ModuleDirectory();
    std::error_code ec;
    std::filesystem::copy_file(dir / "DSRUpscaler.log", dir / "DSRUpscaler.prev.log",
                               std::filesystem::copy_options::overwrite_existing, ec);
    log::Init(dir / "DSRUpscaler.log");
    config::Load(dir / "DSRUpscaler.ini");
    const auto& cfg = config::Get();
    log::SetLevel(cfg.logLevel);
    crash::Install();
    log::Info("DSRUpscaler {} ({})", DSRU_VERSION,
#ifdef NDEBUG
              "release"
#else
              "debug"
#endif
    );
    log::Info("Config: upscaler {} {} | frame generation {} ({}, {}x, presenter {}) | V-Sync {}",
              cfg.upscalerEnabled ? "on" : "off", ToString(cfg.backend), cfg.frameGeneration ? "on" : "off",
              ToString(cfg.frameGenerationBackend), cfg.generatedFrames + 1, cfg.presenter ? "on" : "off", cfg.vsync);
    game::LogGameInfo();
    if (!cfg.enabled) {
        log::Info("Disabled in config - running as plain dxgi pass-through");
        return;
    }
    if (!hooks::Initialize()) {
        log::Error("Hook manager failed to initialize - running as plain dxgi pass-through");
        return;
    }
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        hooks::Install(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlExitUserProcess")), &hkRtlExitUserProcess,
                       &oRtlExitUserProcess, "ntdll!RtlExitUserProcess");
    // The frame rate limit is set live only through DS1 FPS Unlock, which keeps the game's speed.
    const game::fps_unlock_mod::Info& mod = game::fps_unlock_mod::Detect();
    game::high_fps::Install(cfg.targetFps, mod.loaded && mod.variableFrameTime ? mod.maxFps : 0);
}

}  // namespace

void EnsureInitialized() { std::call_once(g_initOnce, Initialize); }

}  // namespace dsru::core

namespace dsru::proxy {

void EnsureInitialized() { core::EnsureInitialized(); }

std::wstring ChainLoadPath() { return config::Get().chainLoadDxgi; }

void OnFactoryCreated(IUnknown* factory) { hooks::dxgi::OnFactoryCreated(factory); }

}  // namespace dsru::proxy
