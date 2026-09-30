#include "overlay/overlay.h"

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/perf.h"
#include "framegen/framegen.h"
#include "game/high_fps.h"
#include "hooks/hook_manager.h"
#include "render/presenter.h"
#include "render/upscale.h"
#include "upscaler/fsr/fsr.h"
#include "upscaler/upscaler.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <wrl/client.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>
#include <cstring>
#include <format>
#include <intrin.h>
#include <mutex>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace dsru::overlay {

namespace {

std::recursive_mutex g_mutex;  // ImGui: the window thread (WndProc) and the present thread
bool g_initialized = false;
bool g_failed = false;
bool g_menuOpen = false;
bool g_menuKeyDown = false;
HWND g_hwnd = nullptr;
WNDPROC g_originalWndProc = nullptr;

// --- Input held back while the menu is open ---
std::atomic<bool> g_captureInput{false};
POINT g_frozenCursor{};
uintptr_t g_selfBegin = 0, g_selfEnd = 0;

using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);
SetCursorPos_t oSetCursorPos = nullptr;
ClipCursor_t oClipCursor = nullptr;
GetCursorPos_t oGetCursorPos = nullptr;

bool FromGame(void* returnAddress) {
    const auto a = reinterpret_cast<uintptr_t>(returnAddress);
    return a < g_selfBegin || a >= g_selfEnd;
}

BOOL WINAPI hkSetCursorPos(int x, int y) {
    if (g_captureInput && FromGame(_ReturnAddress())) return TRUE;
    return oSetCursorPos(x, y);
}
BOOL WINAPI hkClipCursor(const RECT* rect) {
    if (g_captureInput && FromGame(_ReturnAddress())) return TRUE;
    return oClipCursor(rect);
}
BOOL WINAPI hkGetCursorPos(LPPOINT point) {
    if (g_captureInput && point && FromGame(_ReturnAddress())) {
        *point = g_frozenCursor;
        return TRUE;
    }
    return oGetCursorPos(point);
}

using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
GetDeviceState_t oGetDeviceState = nullptr;
GetDeviceData_t oGetDeviceData = nullptr;

HRESULT STDMETHODCALLTYPE hkGetDeviceState(IDirectInputDevice8W* self, DWORD size, LPVOID data) {
    const HRESULT hr = oGetDeviceState(self, size, data);
    if (g_captureInput && SUCCEEDED(hr) && data) std::memset(data, 0, size);
    return hr;
}
HRESULT STDMETHODCALLTYPE hkGetDeviceData(IDirectInputDevice8W* self, DWORD objectSize, LPDIDEVICEOBJECTDATA data,
                                          LPDWORD count, DWORD flags) {
    const HRESULT hr = oGetDeviceData(self, objectSize, data, count, flags);
    if (g_captureInput && SUCCEEDED(hr) && count) *count = 0;
    return hr;
}

void InstallInputHooks() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(paths::SelfModule());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    g_selfBegin = reinterpret_cast<uintptr_t>(dos);
    g_selfEnd = g_selfBegin + nt->OptionalHeader.SizeOfImage;
    hooks::Batch batch;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        hooks::Install(reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")), &hkSetCursorPos, &oSetCursorPos,
                       "user32!SetCursorPos");
        hooks::Install(reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor")), &hkClipCursor, &oClipCursor,
                       "user32!ClipCursor");
        hooks::Install(reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos")), &hkGetCursorPos, &oGetCursorPos,
                       "user32!GetCursorPos");
    }
    // Every DirectInput device shares one implementation, found through a device of our own.
    HMODULE dinput = LoadLibraryW(L"dinput8.dll");
    using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create = dinput ? reinterpret_cast<Create_t>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
    ComPtr<IDirectInput8W> input;
    ComPtr<IDirectInputDevice8W> device;
    static const GUID kIID_IDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID kGUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kIID_IDirectInput8W,
                                 reinterpret_cast<void**>(input.GetAddressOf()), nullptr)) ||
        FAILED(input->CreateDevice(kGUID_SysMouse, &device, nullptr))) {
        log::Warn("Overlay: DirectInput unavailable - the game may still see input while the menu is open");
        return;
    }
    hooks::Install(hooks::VTableEntry(device.Get(), 9), &hkGetDeviceState, &oGetDeviceState,
                   "IDirectInputDevice8::GetDeviceState");
    hooks::Install(hooks::VTableEntry(device.Get(), 10), &hkGetDeviceData, &oGetDeviceData,
                   "IDirectInputDevice8::GetDeviceData");
}

void SetMenuOpen(bool open) {
    if (open == g_menuOpen) return;
    g_menuOpen = open;
    ImGui::GetIO().MouseDrawCursor = open;
    if (open) {
        if (oGetCursorPos) oGetCursorPos(&g_frozenCursor);
        g_captureInput = true;
        if (oClipCursor) oClipCursor(nullptr);
    } else {
        g_captureInput = false;
        if (oSetCursorPos) oSetCursorPos(g_frozenCursor.x, g_frozenCursor.y);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard lock(g_mutex);
        if (g_initialized && g_menuOpen) {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            const bool input = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                               msg == WM_INPUT;
            if (input) return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wParam, lParam) : 0;
        }
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
}

bool Initialize(ID3D11DeviceContext* context, HWND window, UINT height) {
    if (g_initialized || g_failed) return g_initialized;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing to persist; our settings go to DSRUpscaler.ini
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableKeyboard;

    // Sized for the output resolution: 1080p = 1.0.
    const float scale = std::max(1.0f, static_cast<float>(height) / 1080.0f);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowRounding = 6.0f * scale;
    style.MouseCursorScale = scale;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX11_Init(device.Get(), context)) {
        log::Error("Overlay: ImGui initialization failed");
        g_failed = true;
        return false;
    }
    g_hwnd = window;
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    if (!g_originalWndProc) log::Warn("Overlay: could not subclass the game window - no mouse/keyboard input");
    InstallInputHooks();
    g_initialized = true;
    log::Info("Overlay: ready (scale {:.2f}, menu key 0x{:02X})", scale, config::Get().overlayKey);
    return true;
}

// --- UI ---

const ImVec4 kGreen{0.45f, 0.9f, 0.45f, 1.0f}, kYellow{1.0f, 0.8f, 0.3f, 1.0f}, kGrey{0.7f, 0.7f, 0.7f, 1.0f};

// Frames per second: the game's, and on screen (frame generation presents framesPresented frames per
// game frame).
struct Fps {
    float game = 0.0f;
    float shown = 0.0f;
    bool generated = false;
};
Fps CurrentFps() {
    const perf::Stats ps = perf::Get();
    Fps f;
    f.game = ps.frameMs > 0 ? 1000.0f / ps.frameMs : 0.0f;
    f.shown = f.game;
    const framegen::Status fg = framegen::GetStatus();
    if (fg.active && fg.framesPresented > 1) {
        f.shown = f.game * static_cast<float>(fg.framesPresented);
        f.generated = true;
    }
    return f;
}

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void StatusLine(const render::upscale::Status& s) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 10.0f * ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos({vp->WorkPos.x + pad, vp->WorkPos.y + pad});
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##status", nullptr, flags)) {
        const char* backend = ToString(s.backend);
        if (!s.enabled)
            ImGui::TextColored(kGrey, "%s OFF (game TAA)", backend);
        else if (s.active)
            ImGui::TextColored(kGreen, "%s %s %ux%u -> %ux%u", s.description.c_str(), ModeName(s.backend, s.quality),
                               s.width, s.height, s.outputWidth, s.outputHeight);
        else
            ImGui::TextColored(kYellow, "%s inactive: %s", backend, s.failed ? "stopped after errors" : s.reason);
        if (s.dlss5Active) {
            ImGui::SameLine();
            ImGui::TextColored(kGreen, "| DLSS 5");
        }
        ImGui::SameLine();
        const Fps fps = CurrentFps();
        if (fps.generated)
            ImGui::TextColored(kGreen, "| %.0f FPS (game %.0f + frame gen)", fps.shown, fps.game);
        else
            ImGui::Text("| %.0f FPS", fps.game);
        ImGui::SameLine();
        ImGui::TextDisabled("| [Home] menu");
    }
    ImGui::End();
}

// The game's resolution for `mode` on this output (the mode follows the game's resolution setting).
std::string ResolutionFor(const render::upscale::Status& s, QualityMode mode) {
    UINT gameW = 0, gameH = 0, outW = 0, outH = 0;
    if (!render::presenter::Sizes(gameW, gameH, outW, outH)) outW = 3840, outH = 2160;
    const float scale = NominalScale(s.backend, mode);
    return std::format("{}x{}", static_cast<int>(outW * scale + 0.5f), static_cast<int>(outH * scale + 0.5f));
}

void SettingsWindow(const render::upscale::Status& s) {
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowPos({60 * scale, 80 * scale}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({560 * scale, 0}, ImGuiCond_FirstUseEver);
    bool open = true;  // the title bar's close button goes through SetMenuOpen (input capture)
    if (!ImGui::Begin("DSRUpscaler " DSRU_VERSION, &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!open) SetMenuOpen(false);
        return;
    }

    ImGui::SeparatorText("Upscaler");
    bool enabled = s.enabled;
    if (ImGui::Checkbox("Upscaler (off = the game's own TAA)   [F7]", &enabled)) render::upscale::SetEnabled(enabled);
    if (ImGui::BeginCombo("Upscaler", ToString(s.backend))) {
        for (Backend b : kBackends) {
            const bool unavailable = upscaler::Probed(b) && !upscaler::Available(b);
            const std::string label = std::string(ToString(b)) + (unavailable ? "  (unavailable)" : "");
            if (ImGui::Selectable(label.c_str(), b == s.backend)) render::upscale::SetBackend(b);
        }
        ImGui::EndCombo();
    }
    Help("DLSS: NVIDIA RTX.\nFSR: any GPU (FSR 4 on the AMD GPUs whose driver offers it).\n"
         "XeSS: Intel Arc, and other GPUs with Shader Model 6.4.");

    // Modes: the game renders at its resolution setting, the upscaler brings it to the screen.
    static QualityMode wanted = config::Get().quality;
    const auto modeLabel = [&](QualityMode m) {
        if (m == QualityMode::Native) return std::format("{} (native resolution)", ModeName(s.backend, m));
        return std::format("{} ({:.0f}%, game at {})", ModeName(s.backend, m), 100.0f * NominalScale(s.backend, m),
                           ResolutionFor(s, m));
    };
    if (ImGui::BeginCombo("Mode", modeLabel(SupportedMode(s.backend, wanted)).c_str())) {
        for (QualityMode m : Modes(s.backend))
            if (ImGui::Selectable(modeLabel(m).c_str(), m == wanted)) {
                wanted = m;
                config::SaveUpscaler(s.enabled, s.backend, m);
            }
        ImGui::EndCombo();
    }
    Help("The render resolution is the game's own setting (Options > Graphics > Resolution): below your screen's, the "
         "game renders smaller and the upscaler brings it to the screen, the HUD stays sharp. The mode adapts to the "
         "resolution you pick; the closest one the game offers works.");
    const QualityMode wantedMode = SupportedMode(s.backend, wanted);
    if (s.active && wantedMode != s.quality)
        ImGui::TextColored(kYellow, "Set the game's resolution to %s for %s (now %ux%u: %s).",
                           wantedMode == QualityMode::Native ? std::format("{}x{}", s.outputWidth, s.outputHeight).c_str()
                                                             : ResolutionFor(s, wantedMode).c_str(),
                           ModeName(s.backend, wantedMode), s.width, s.height, ModeName(s.backend, s.quality));

    if (s.backend == Backend::Dlss) {
        const DlssPresetInfo* current = &DlssPresets()[0];
        for (const DlssPresetInfo& p : DlssPresets())
            if (p.value == s.dlssPreset) current = &p;
        if (ImGui::BeginCombo("Preset", current->name)) {
            for (const DlssPresetInfo& p : DlssPresets()) {
                if (ImGui::Selectable(p.name, &p == current)) render::upscale::SetDlssPreset(p.value);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.description);
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("%s", current->description);
    } else if (s.backend == Backend::Fsr && fsr::Available()) {
        const std::string current = s.fsrVersion.empty() ? std::string("Newest") : s.fsrVersion;
        if (ImGui::BeginCombo("Version", current.c_str())) {
            if (ImGui::Selectable("Newest", s.fsrVersion.empty())) render::upscale::SetFsrVersion("");
            for (const fsr::Version& v : fsr::Versions())
                if (ImGui::Selectable(v.name.c_str(), v.name == s.fsrVersion)) render::upscale::SetFsrVersion(v.name);
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("FSR versions this GPU's driver / runtime offers (FSR 4 needs an AMD Radeon RX 9000).");
    }
    if (!s.taaSeen) ImGui::TextColored(kYellow, "Set Options > Graphics > Anti-Aliasing to TXAA: the upscaler replaces it.");

    ImGui::SeparatorText("Frame Generation");
    const framegen::Status fg = framegen::GetStatus();
    static Backend nextBackend = config::Get().frameGenerationBackend;
    const auto fgName = [](Backend b) {
        switch (b) {
            case Backend::Dlss: return "DLSS-G (NVIDIA RTX 40 and newer)";
            case Backend::Fsr: return "FSR (any GPU)";
            case Backend::Xess: return "XeSS-FG (Intel Arc, other GPUs with Shader Model 6.4)";
        }
        return "?";
    };
    if (ImGui::BeginCombo("Backend##fg", fgName(nextBackend))) {
        for (Backend b : kBackends)
            if (ImGui::Selectable(fgName(b), b == nextBackend) && b != nextBackend) {
                nextBackend = b;
                config::SaveFrameGenerationBackend(b);
            }
        ImGui::EndCombo();
    }
    if (nextBackend != fg.backend)
        ImGui::TextColored(kYellow, "Restart the game to switch to %s (this session: %s).", ToString(nextBackend),
                           ToString(fg.backend));
    if (!fg.attached) {
        ImGui::TextDisabled("%s frame generation is not attached (see DSRUpscaler.log; needs Presenter=1).",
                            ToString(fg.backend));
    } else {
        bool fgOn = fg.enabled;
        int frames = static_cast<int>(fg.framesToGenerate);
        bool fgChanged = ImGui::Checkbox("Frame Generation", &fgOn);
        const int maxFrames = fg.maxFramesToGenerate ? static_cast<int>(fg.maxFramesToGenerate) : 3;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f * scale);
        const std::string multiplier = std::to_string(frames + 1) + "x";
        if (ImGui::BeginCombo("##fgframes", multiplier.c_str())) {
            for (int f = 1; f <= maxFrames; ++f)
                if (ImGui::Selectable((std::to_string(f + 1) + "x").c_str(), f == frames)) {
                    frames = f;
                    fgChanged = true;
                }
            ImGui::EndCombo();
        }
        if (fgChanged) framegen::SetByUser(fgOn, static_cast<uint32_t>(frames));
        if (fg.active)
            ImGui::Text("Active: %u frames presented per game frame%s", fg.framesPresented,
                        fg.status ? " - frame generation reports a problem (see log)" : "");
        else if (fg.enabled)
            ImGui::TextDisabled("Waiting for gameplay (TXAA on; off in menus and loading screens).");
    }
    ImGui::TextDisabled("Keep game FPS x multiplier at or below the refresh rate (Frame rate below).");

    ImGui::SeparatorText("DLSS 5 (neural rendering)");
    bool dlss5 = s.dlss5Enabled;
    float dlss5Intensity = s.dlss5Intensity;
    int dlss5Style = s.dlss5Style;
    bool dlss5Changed = ImGui::Checkbox("DLSS 5   [F8]", &dlss5);
    ImGui::BeginDisabled(!dlss5);
    dlss5Changed |= ImGui::SliderFloat("Intensity", &dlss5Intensity, 0.0f, 2.0f, "%.2f");
    dlss5Changed |= ImGui::SliderInt("Style", &dlss5Style, 0, 7);
    ImGui::EndDisabled();
    if (dlss5Changed) render::upscale::SetDlss5(dlss5, dlss5Intensity, dlss5Style);
    if (s.dlss5Reason[0])
        ImGui::TextColored(kYellow, "Unavailable: %s", s.dlss5Reason);
    else if (s.dlss5Active)
        ImGui::TextColored(kGreen, "Active on the HUD-less scene");
    else if (s.dlss5Enabled)
        ImGui::TextDisabled("Waiting for gameplay (needs TXAA).");
    ImGui::TextDisabled("Needs the DLSS 5 runtime (nvngx_dlssnr.dll, not included) in DSRUpscaler\\ - RTX 50.");

    ImGui::SeparatorText("Frame rate");
    const game::high_fps::Status hf = game::high_fps::GetStatus();
    if (!hf.available) {
        ImGui::TextColored(kYellow, "Unavailable: %s", hf.reason);
    } else {
        // Applied when the slider is released (each change restarts the engine's frame clock).
        static int fps = config::Get().targetFps;
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderInt("Game FPS limit", &fps, game::high_fps::kGameFps, 360, fps <= 60 ? "60 (the game's)" : "%d");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            game::high_fps::SetTarget(fps);
            config::SaveTargetFps(fps);
        }
        if (hf.active)
            if (hf.limit < hf.target)
                ImGui::TextColored(kGreen, "Unlocked, held at %d FPS (the GPU's pace, no judder), game speed kept", hf.limit);
            else
                ImGui::TextColored(kGreen, "Unlocked at %d FPS, game speed kept", hf.target);
        else if (hf.reason[0])
            ImGui::TextDisabled("%s", hf.reason);
        ImGui::TextDisabled("The game keeps its speed from 60 FPS up to the limit; below 60 it slows down as the original does.");
    }

    ImGui::SeparatorText("Status");
    if (s.active)
        ImGui::Text("Active: %s %s, %ux%u -> %ux%u", s.description.c_str(), ModeName(s.backend, s.quality), s.width,
                    s.height, s.outputWidth, s.outputHeight);
    else
        ImGui::TextWrapped("Inactive: %s", s.failed ? "stopped after errors (F7 twice to retry)" : s.reason);

    // Developer tools: LogLevel=debug.
    if (log::Enabled(log::Level::Debug) && ImGui::CollapsingHeader("Diagnostics")) {
        ImGui::Text("TAA resolve seen: %s   velocity pass seen: %s   deferred split: %s", s.taaSeen ? "yes" : "no",
                    s.velocitySeen ? "yes" : "no", s.splitLists ? "yes" : "no");
        ImGui::Text("Jitter: %+.3f, %+.3f px   near %.3f far %.1f   fov %.1f deg", s.jitterX, s.jitterY, s.nearPlane,
                    s.farPlane, s.fovY * 57.2958f);
        bool fx = s.flipX, fy = s.flipY;
        bool changed = ImGui::Checkbox("Flip jitter X", &fx);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip jitter Y", &fy);
        if (changed) render::upscale::SetJitterFlip(fx, fy);
        ImGui::TextDisabled("Stand still in Performance / Ultra Performance: the sharpest setting is the right one.");
    }

    ImGui::SeparatorText("Performance");
    const perf::Stats ps = perf::Get();
    if (const Fps fps = CurrentFps(); fps.generated)
        ImGui::Text("On screen: %.0f FPS with frame generation (%.1f game FPS)", fps.shown, fps.game);
    ImGui::Text("Game FPS: %.1f  frame %.2f ms = game %.2f + mod %.2f + GPU/V-Sync wait %.2f",
                ps.frameMs > 0 ? 1000.0f / ps.frameMs : 0.0f, ps.frameMs, ps.gameMs, ps.modMs, ps.waitMs);

    ImGui::SeparatorText("Overlay");
    bool statusLine = config::Get().overlayStatusLine;
    if (ImGui::Checkbox("Show the status line", &statusLine)) config::SaveOverlayStatusLine(statusLine);
    ImGui::TextDisabled("Home: this window   F7: upscaler on/off   F8: DLSS 5 on/off");
    ImGui::TextDisabled("Every setting is saved to DSRUpscaler.ini as it changes.");
    ImGui::End();
    if (!open) SetMenuOpen(false);
}

}  // namespace

void Render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width, UINT height, HWND window) {
    if (!context || !target || !window) return;
    std::lock_guard lock(g_mutex);
    if (!Initialize(context, window, height)) return;

    // Menu key (game window focused), polled like the other hotkeys.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(config::Get().overlayKey) & 0x8000);
    if (down && !g_menuKeyDown) SetMenuOpen(!g_menuOpen);
    g_menuKeyDown = down;

    const bool statusLine = config::Get().overlayStatusLine;
    if (!g_menuOpen && !statusLine) return;

    const render::upscale::Status status = render::upscale::GetStatus();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    ImGui::NewFrame();
    if (statusLine) StatusLine(status);
    if (g_menuOpen) SettingsWindow(status);
    ImGui::Render();

    // The backend restores the pipeline state it changes, except the render targets set here.
    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* savedDsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, &savedDsv);
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, savedDsv);
    for (auto* v : savedRtvs)
        if (v) v->Release();
    if (savedDsv) savedDsv->Release();
}

void OnBeforeResize() {}

}  // namespace dsru::overlay
