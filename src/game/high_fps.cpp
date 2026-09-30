#include "game/high_fps.h"

#include "core/log.h"
#include "game/pattern_scan.h"
#include "game/rtti.h"
#include "hooks/hook_manager.h"

#include <Windows.h>
#include <intrin.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dsru::game::high_fps {

namespace {

// ---------------------------------------------------------------------------------------------------
// Where things are (DarkSoulsRemastered.exe 2022, offsets checked against it)
// ---------------------------------------------------------------------------------------------------

// Flipper factory, mode 5: GetFrameClock(); mov edx, 140; jmp -> SetRate(clock, edx, 0).
// GetFrameClock is `mov rax, [global]; test rax, rax; je ...; ret`.
constexpr const char* kClockPattern = "E8 ?? ?? ?? ?? BA 8C 00 00 00 EB";
// MoveMapStep's constructor: lea rax, [step table]; mov [r14+20h], rax; lea rax, [vtable]; mov [r14], rax.
constexpr const char* kMoveMapPattern = "41 88 6E 1C 48 8D 05 ?? ?? ?? ?? 49 89 46 20 48 8D 05 ?? ?? ?? ?? 49 89 06";
// The cutscene step table's static initializer: xor edx, edx; lea rcx, [table]; lea r8d, [rdx+78h]; call memset.
constexpr const char* kRemoPattern = "33 D2 48 8D 0D ?? ?? ?? ?? 44 8D 42 78 E8";
// The limiter's wait, WaitUntil(clock, ticks): rate = clock+60h; ...; QPC (returns at +1Eh); loop: QPC
// (returns at +54h) until (QPC - start) * rate / frequency >= ticks.
constexpr const char* kWaitPattern = "8B 41 60 8B F2 48 8B F9 85 C0 74 ?? 48 89 5C 24 38 48 8D 4C 24 30 8B D8 FF 15";
constexpr size_t kWaitFirstReturn = 0x1E, kWaitLoopReturn = 0x54;
// Main update, Havok section: ...; mov edx, 5; call ProfileBegin; movaps xmm1, xmm6; mov rcx, [world];
// call HavokStep (the call is at +38; the step itself is in the protected section).
constexpr const char* kHavokPattern =
    "BA 04 00 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 BA 05 00 00 00 E8 ?? ?? ?? ?? 0F 28 CE 48 8B 0D ?? ?? ?? ?? E8";
constexpr size_t kHavokCallOffset = 38;
// Menu action query (menu object, action id) -> pressed: its prologue.
constexpr const char* kMenuQueryPattern =
    "40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 8B DA F6 81 7C 01 00 00 01";

constexpr size_t kMoveMapUpdateStep = 13;
constexpr size_t kRemoUpdateStep = 9;
constexpr size_t kFxUpdateSlot = 5;

// The limiter waits until (QPC - start) * rate / frequency >= ticks, then ++ticks (one per frame).
constexpr size_t kClockRate = 0x60, kClockStart = 0x678, kClockTicks = 0x690;

// Code patches wait this long after Install (the protection's checks run at startup).
constexpr ULONGLONG kCodePatchDelayMs = 5000;
// Presses closer together than this are one press (both detectors of a button see it).
constexpr ULONGLONG kPressMergeMs = 41;
// A menu query answered "pressed" this long after the press is a held button: let it through.
constexpr ULONGLONG kPressWindowMs = 250;

// ---------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------

std::atomic<int> g_target{kGameFps};
// The simulation's last time step / (1/60), for the mouse (see StepScale).
std::atomic<float> g_dtScale{1.0f};
std::atomic<uint64_t> g_simulationSteps{0};  // diagnostics: simulation updates vs presented frames
int64_t g_qpcFrequency = 0;
std::atomic<bool> g_active{false};
bool g_available = false;
const char* g_reason = "not installed";
ULONGLONG g_installTick = 0;

uintptr_t g_image = 0;


uintptr_t g_clockGlobal = 0;  // address of the frame clock pointer
int g_appliedTarget = 0;  // what the engine was last set to (0: never touched)
// The clock's rate: the target, or lower while the GPU can't keep up (see AdaptLimit).
std::atomic<int> g_limit{kGameFps};
// Time the limiter spent waiting (QPC ticks, all frames): unused capacity. The executable's
// QueryPerformanceCounter import is hooked; only the two calls in the wait loop are looked at.
uintptr_t g_waitFirst = 0, g_waitLoop = 0;
int64_t g_waitPrevious = 0;
std::atomic<int64_t> g_waitTotal{0};
bool g_waitMeasured = false;
using Qpc_t = BOOL(WINAPI*)(LARGE_INTEGER*);
Qpc_t oQpc = nullptr;

BOOL WINAPI hkQpc(LARGE_INTEGER* counter) {
    const BOOL ok = oQpc(counter);
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (ok && counter) {
        if (caller == g_waitLoop) {
            g_waitTotal.fetch_add(counter->QuadPart - g_waitPrevious, std::memory_order_relaxed);
            g_waitPrevious = counter->QuadPart;
        } else if (caller == g_waitFirst) {
            g_waitPrevious = counter->QuadPart;
        }
    }
    return ok;
}

uintptr_t g_remoTable = 0;
bool g_remoHooked = false;
uintptr_t g_havokCall = 0, g_menuQuery = 0;
std::atomic<bool> g_codePatched{false};

float Scale() { return g_dtScale.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------------------------------
// Memory helpers (SEH: game objects can go away under us)
// ---------------------------------------------------------------------------------------------------

template <class T>
bool Read(uintptr_t address, T& out) {
    __try {
        out = *reinterpret_cast<const volatile T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Write(uintptr_t address, T value) {
    __try {
        *reinterpret_cast<volatile T*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t Relative(uintptr_t instruction, size_t offset, size_t length) {
    return ResolveRelative(instruction, offset, length);
}

uintptr_t Rva(uintptr_t address) { return address - g_image; }

// A vtable-like slot (in .rdata or .data) replaced by `detour`.
bool HookSlot(uintptr_t slot, void* detour, void** original, const char* name) {
    void* table = reinterpret_cast<void*>(slot);
    return hooks::InstallVTableRaw(&table, 0, detour, original, name);
}

// ---------------------------------------------------------------------------------------------------
// Time steps
// ---------------------------------------------------------------------------------------------------

using Step_t = uintptr_t(__fastcall*)(void*, float);
using RemoStep_t = uintptr_t(__fastcall*)(void*, float, void*, void*);

Step_t oMoveMapUpdate = nullptr, oFxUpdate = nullptr, oHavokStep = nullptr;
RemoStep_t oRemoUpdate = nullptr;

// Time step / (1/60) for one updater (a hooked function and its object): the real time since it last
// ran, on the thread that runs it, at most 1/60 s (the game never ran with longer steps: below 60 FPS
// it slows down as the original does). Not the presented frame rate: the simulation thread can run
// several updates per presented frame when the GPU is the limit.
float StepScale(const void* hook, const void* self) {
    struct Key {
        const void* hook;
        const void* self;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        size_t operator()(const Key& k) const { return std::hash<const void*>()(k.hook) * 31 ^ std::hash<const void*>()(k.self); }
    };
    static std::mutex mutex;
    static std::unordered_map<Key, int64_t, Hash> last;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard lock(mutex);
    if (last.size() > 1024) last.clear();
    int64_t& previous = last[{hook, self}];
    const int64_t elapsed = previous ? now.QuadPart - previous : 0;
    previous = now.QuadPart;
    if (!elapsed) return static_cast<float>(kGameFps) / static_cast<float>(g_target.load());  // first step: the target's
    const double scale = static_cast<double>(elapsed) * kGameFps / static_cast<double>(g_qpcFrequency);
    return static_cast<float>(std::clamp(scale, 0.01, 1.0));
}

// Scale of the step `what` gets now (1 while the unlock is off), logged the first time.
float Step(std::atomic<bool>& logged, const char* what, const void* hook, const void* self, float incoming) {
    if (!g_active.load(std::memory_order_relaxed)) return 1.0f;
    const float scale = StepScale(hook, self);
    if (!logged.exchange(true))
        log::Info("High FPS: {} time step {:.6f} -> {:.6f} s", what, incoming, incoming * scale);
    return scale;
}

void PatchCode();

uintptr_t __fastcall hkMoveMapUpdate(void* self, float dt) {
    static std::atomic<bool> logged{false};
    const float scale = Step(logged, "simulation", &hkMoveMapUpdate, self, dt);
    if (g_active.load(std::memory_order_relaxed)) {
        g_dtScale.store(scale, std::memory_order_relaxed);
        g_simulationSteps.fetch_add(1, std::memory_order_relaxed);
        if (!g_codePatched.load(std::memory_order_relaxed) && GetTickCount64() - g_installTick >= kCodePatchDelayMs)
            PatchCode();
    }
    return oMoveMapUpdate(self, dt * scale);
}

uintptr_t __fastcall hkRemoUpdate(void* self, float dt, void* a3, void* a4) {
    static std::atomic<bool> logged{false};
    return oRemoUpdate(self, dt * Step(logged, "cutscene", &hkRemoUpdate, self, dt), a3, a4);
}

uintptr_t __fastcall hkFxUpdate(void* self, float dt) {
    static std::atomic<bool> logged{false};
    return oFxUpdate(self, dt * Step(logged, "effects", &hkFxUpdate, self, dt));
}

// Reached from the relay the Havok call site now calls.
uintptr_t __fastcall HavokStep(void* world, float dt) {
    static std::atomic<bool> logged{false};
    return oHavokStep(world, dt * Step(logged, "physics", &HavokStep, world, dt));
}

// ---------------------------------------------------------------------------------------------------
// Input: repeat delays, one menu action per press
// ---------------------------------------------------------------------------------------------------

// DLUserInput{Press,Repeat}Detector::Update(self, output, ?, input, dt): walks the input's action list
// (helper(input, 0): vector at +0x10 / +0x18) and sets the output's bitset (one bit per action id).
using Detector_t = void(__fastcall*)(void*, void*, void*, void*, float);
using ActionList_t = void*(__fastcall*)(void*, int);

struct Detector {
    const char* name;
    size_t slot;
    size_t stride;      // action list entry size
    size_t idOffset;    // action id in an entry
    size_t bitsOffset;  // output bitset pointer
    bool repeat;
    Detector_t original = nullptr;
    ActionList_t list = nullptr;
};
std::array<Detector, 4> g_detectors = {{
    {"press", 4, 8, 4, 0x198, false},
    {"press (alternate)", 5, 8, 4, 0x50, false},
    {"repeat", 4, 12, 8, 0x198, true},
    {"repeat (alternate)", 5, 12, 8, 0x50, true},
}};

std::mutex g_pressMutex;
struct DetectorKey {
    void* self;
    int kind;
    bool operator==(const DetectorKey&) const = default;
};
struct DetectorKeyHash {
    size_t operator()(const DetectorKey& k) const { return std::hash<void*>()(k.self) ^ static_cast<size_t>(k.kind); }
};
std::unordered_map<DetectorKey, std::array<uint32_t, 8>, DetectorKeyHash> g_down;  // bits last seen set
std::array<uint64_t, 256> g_pressSeq{};   // press number of each action's last press (0: none)
std::array<ULONGLONG, 256> g_pressTick{};
uint64_t g_seq = 0;
ULONGLONG g_lastPress = 0;
std::unordered_map<void*, uint64_t> g_menuSeen;  // menu object -> press it acted on

float RepeatStep(void* self, float dt) {
    if (!g_active.load(std::memory_order_relaxed)) return dt;
    const float scale = StepScale(&RepeatStep, self);
    if (scale >= 1.0f) return dt;
    // Only the engine's fixed 1/60 is corrected (not a time step that already is the real one).
    const float nominal = 1.0f / kGameFps;
    return std::fabs(dt - nominal * scale) > std::fabs(dt - nominal) ? dt * scale : dt;
}

// Called with g_pressMutex held.
void Pressed(uint32_t id) {
    const ULONGLONG now = GetTickCount64();
    if (!g_seq || now - g_lastPress >= kPressMergeMs) {
        ++g_seq;
        g_lastPress = now;
    }
    g_pressSeq[id] = g_seq;
    g_pressTick[id] = now;
}

void RecordPresses(int kind, void* self, void* output, void* input) {
    const Detector& d = g_detectors[kind];
    if (!g_active.load(std::memory_order_relaxed) || !self || !output || !input || !d.list) return;
    const auto list = reinterpret_cast<uintptr_t>(d.list(input, 0));
    if (!list) return;
    const auto begin = *reinterpret_cast<const uint8_t* const*>(list + 0x10);
    const auto end = *reinterpret_cast<const uint8_t* const*>(list + 0x18);
    const auto bits = *reinterpret_cast<const uint32_t* const*>(static_cast<uint8_t*>(output) + d.bitsOffset);
    if (!begin || !bits || end < begin || end - begin > 0x1000) return;
    std::lock_guard lock(g_pressMutex);
    auto& down = g_down[{self, kind}];
    for (const uint8_t* entry = begin; entry + d.stride <= end; entry += d.stride) {
        const auto id = *reinterpret_cast<const uint32_t*>(entry + d.idOffset);
        if (id > 255) continue;
        const uint32_t bit = 1u << (id & 31);
        if (bits[id >> 5] & bit) {
            if (!(down[id >> 5] & bit)) Pressed(id);
            down[id >> 5] |= bit;
        } else {
            down[id >> 5] &= ~bit;
        }
    }
}

template <int I>
void __fastcall hkDetector(void* self, void* output, void* a3, void* input, float dt) {
    Detector& d = g_detectors[I];
    if (d.repeat) {
        static std::atomic<bool> logged{false};
        const float corrected = RepeatStep(self, dt);
        if (corrected != dt && !logged.exchange(true))
            log::Info("High FPS: input repeat time step {:.6f} -> {:.6f} s", dt, corrected);
        dt = corrected;
    }
    d.original(self, output, a3, input, dt);
    RecordPresses(I, self, output, input);
}

using MenuQuery_t = bool(__fastcall*)(void*, uint32_t);
MenuQuery_t oMenuQuery = nullptr;

bool __fastcall hkMenuQuery(void* self, uint32_t action) {
    const bool pressed = oMenuQuery(self, action);
    if (!pressed || !self || action > 255 || !g_active.load(std::memory_order_relaxed)) return pressed;
    std::lock_guard lock(g_pressMutex);
    const uint64_t seq = g_pressSeq[action];
    if (!seq || GetTickCount64() - g_pressTick[action] > kPressWindowMs) return pressed;
    if (g_menuSeen.size() > 4096) g_menuSeen.clear();
    uint64_t& seen = g_menuSeen[self];
    if (seen == seq) return false;  // this press already moved this menu
    seen = seq;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Mouse (DirectInput): the camera turns by delta x time step
// ---------------------------------------------------------------------------------------------------

std::mutex g_mouseMutex;
std::unordered_map<void*, std::array<double, 2>> g_mouseRemainder;
std::unordered_map<void*, bool> g_isMouse;

// raw / time step scale (the camera turns by delta x step), the remainder carried to the next read.
LONG ScaleAxis(LONG raw, double& remainder) {
    const double v = raw / static_cast<double>(Scale()) + remainder;
    const double out = std::trunc(v);
    remainder = v - out;
    if (out > LONG_MAX || out < LONG_MIN) {
        remainder = 0;
        return out > 0 ? LONG_MAX : LONG_MIN;
    }
    return static_cast<LONG>(out);
}

template <class Device>
bool IsMouse(Device* device) {
    std::lock_guard lock(g_mouseMutex);
    if (const auto it = g_isMouse.find(device); it != g_isMouse.end()) return it->second;
    DIDEVCAPS caps{sizeof(caps)};
    const bool mouse = SUCCEEDED(device->GetCapabilities(&caps)) && GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_MOUSE;
    g_isMouse[device] = mouse;
    return mouse;
}

template <class Device>
struct MouseHooks {
    using State_t = HRESULT(STDMETHODCALLTYPE*)(Device*, DWORD, LPVOID);
    using Data_t = HRESULT(STDMETHODCALLTYPE*)(Device*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
    static inline State_t oState = nullptr;
    static inline Data_t oData = nullptr;

    static HRESULT STDMETHODCALLTYPE State(Device* self, DWORD size, LPVOID data) {
        const HRESULT hr = oState(self, size, data);
        // DIMOUSESTATE / DIMOUSESTATE2 (only mice use these sizes).
        if (FAILED(hr) || !data || (size != sizeof(DIMOUSESTATE) && size != sizeof(DIMOUSESTATE2)) ||
            !g_active.load(std::memory_order_relaxed))
            return hr;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) log::Info("High FPS: mouse movement scaled by the time step (device state)");
        auto* state = static_cast<DIMOUSESTATE*>(data);
        std::lock_guard lock(g_mouseMutex);
        auto& remainder = g_mouseRemainder[self];
        state->lX = ScaleAxis(state->lX, remainder[0]);
        state->lY = ScaleAxis(state->lY, remainder[1]);
        return hr;
    }

    static HRESULT STDMETHODCALLTYPE Data(Device* self, DWORD size, LPDIDEVICEOBJECTDATA items, LPDWORD count,
                                          DWORD flags) {
        const HRESULT hr = oData(self, size, items, count, flags);
        if (FAILED(hr) || !items || !count || (flags & DIGDD_PEEK) || size < sizeof(DIDEVICEOBJECTDATA) ||
            !g_active.load(std::memory_order_relaxed) || !IsMouse(self))
            return hr;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) log::Info("High FPS: mouse movement scaled by the time step (buffered data)");
        std::lock_guard lock(g_mouseMutex);
        auto& remainder = g_mouseRemainder[self];
        for (DWORD i = 0; i < *count; ++i) {
            auto& item = *reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<uint8_t*>(items) + i * size);
            if (item.dwOfs == DIMOFS_X || item.dwOfs == DIMOFS_Y) {
                auto& r = remainder[item.dwOfs == DIMOFS_X ? 0 : 1];
                item.dwData = static_cast<DWORD>(ScaleAxis(static_cast<LONG>(item.dwData), r));
            }
        }
        return hr;
    }
};

// A mouse device of our own gives the device class's vtable (shared with the game's devices, created
// before or after this).
template <class DirectInput, class Device>
bool HookMouse(HMODULE dinput, REFIID iid, const char* name) {
    using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create = reinterpret_cast<Create_t>(GetProcAddress(dinput, "DirectInput8Create"));
    DirectInput* di = nullptr;
    if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, iid, reinterpret_cast<void**>(&di), nullptr)))
        return false;
    Device* device = nullptr;
    bool ok = SUCCEEDED(di->CreateDevice(GUID_SysMouse, &device, nullptr)) && device;
    if (ok) {
        using H = MouseHooks<Device>;
        ok = hooks::InstallVTable(device, 9, &H::State, &H::oState,
                                  (std::string(name) + "::GetDeviceState").c_str()) &&
             hooks::InstallVTable(device, 10, &H::Data, &H::oData, (std::string(name) + "::GetDeviceData").c_str());
        device->Release();
    }
    di->Release();
    return ok;
}

bool InstallMouse() {
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    HMODULE dinput = LoadLibraryW((std::filesystem::path(system) / L"dinput8.dll").c_str());
    if (!dinput) return false;
    const bool w = HookMouse<IDirectInput8W, IDirectInputDevice8W>(dinput, IID_IDirectInput8W, "IDirectInputDevice8W");
    const bool a = HookMouse<IDirectInput8A, IDirectInputDevice8A>(dinput, IID_IDirectInput8A, "IDirectInputDevice8A");
    return w || a;  // dinput8 stays loaded (the game uses it)
}

// ---------------------------------------------------------------------------------------------------
// Code patches (once the game runs)
// ---------------------------------------------------------------------------------------------------

// Memory above `anchor` within a rel32 jump of it. (Not `near`: windef.h defines it away.)
void* AllocateNear(uintptr_t anchor, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t granularity = si.dwAllocationGranularity;
    const uintptr_t high = anchor + 0x7ff00000;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t address = (anchor + granularity - 1) & ~(granularity - 1); address < high;) {
        if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) break;
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (mbi.State == MEM_FREE) {
            const uintptr_t start = (base + granularity - 1) & ~(granularity - 1);
            if (start + size <= base + mbi.RegionSize)
                // Read-write, made executable once written (read-write-execute allocations are refused).
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(start), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
                    return p;
        }
        address = base + mbi.RegionSize;
    }
    return nullptr;
}

// The call site calls a relay next to the executable: mov rax, HavokStep; jmp rax.
void PatchHavok() {
    const uintptr_t call = g_havokCall;
    if (*reinterpret_cast<const uint8_t*>(call) != 0xE8) {
        log::Warn("High FPS: Havok call site changed (0x{:02x}), physics time step not corrected",
                  *reinterpret_cast<const uint8_t*>(call));
        return;
    }
    oHavokStep = reinterpret_cast<Step_t>(Relative(call, 1, 5));
    auto* relay = static_cast<uint8_t*>(AllocateNear(call, 0x1000));
    if (!relay) {
        log::Warn("High FPS: no memory near the executable for the Havok relay, physics time step not corrected");
        return;
    }
    const auto target = reinterpret_cast<uint64_t>(&HavokStep);
    relay[0] = 0x48;
    relay[1] = 0xB8;
    memcpy(relay + 2, &target, sizeof(target));
    relay[10] = 0xFF;
    relay[11] = 0xE0;
    DWORD old = 0;
    if (!VirtualProtect(relay, 0x1000, PAGE_EXECUTE_READ, &old))
        log::Warn("High FPS: VirtualProtect on the Havok relay failed ({})", GetLastError());
    FlushInstructionCache(GetCurrentProcess(), relay, 12);
    // Never point the game at memory it can't run: a crash on the next physics step otherwise.
    MEMORY_BASIC_INFORMATION mbi{};
    constexpr DWORD kExecutable = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    if (!VirtualQuery(relay, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & kExecutable) ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        log::Warn("High FPS: the Havok relay at 0x{:x} isn't executable (protection 0x{:x}), physics time step not "
                  "corrected", reinterpret_cast<uintptr_t>(relay), mbi.Protect);
        return;
    }

    const intptr_t distance = reinterpret_cast<intptr_t>(relay) - static_cast<intptr_t>(call + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) {
        log::Warn("High FPS: the Havok relay at 0x{:x} is out of a call's reach, physics time step not corrected",
                  reinterpret_cast<uintptr_t>(relay));
        return;
    }
    const auto rel = static_cast<int32_t>(distance);
    if (!VirtualProtect(reinterpret_cast<void*>(call), 5, PAGE_EXECUTE_READWRITE, &old)) {
        log::Warn("High FPS: Havok call site not writable ({}), physics time step not corrected", GetLastError());
        return;
    }
    *reinterpret_cast<int32_t*>(call + 1) = rel;
    VirtualProtect(reinterpret_cast<void*>(call), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    log::Info("High FPS: Havok step at exe+0x{:x} relayed from exe+0x{:x} through 0x{:x} (protection 0x{:x})",
              Rva(reinterpret_cast<uintptr_t>(oHavokStep)), Rva(call), reinterpret_cast<uintptr_t>(relay), mbi.Protect);
}

// On the game thread that updates the simulation (the one that runs the Havok call).
void PatchCode() {
    if (g_codePatched.exchange(true)) return;
    PatchHavok();
    if (hooks::Install(reinterpret_cast<void*>(g_menuQuery), &hkMenuQuery, &oMenuQuery, "menu action query"))
        log::Info("High FPS: menu presses filtered (one action per press) at exe+0x{:x}", Rva(g_menuQuery));
}

// ---------------------------------------------------------------------------------------------------
// Engine pacing: the frame clock
// ---------------------------------------------------------------------------------------------------

uintptr_t Clock() {
    uintptr_t clock = 0;
    return Read(g_clockGlobal, clock) ? clock : 0;
}

int ClockRate() {
    uint32_t rate = 0;
    const uintptr_t clock = Clock();
    return clock && Read(clock + kClockRate, rate) ? static_cast<int>(rate) : -1;
}

// The new rate with the schedule's phase kept: the start moves so that the ticks already counted fall
// due now at the new rate (no catch-up burst, no stall). The engine's own SetRate zeroes the start
// instead, which it only seeds again at scene changes: with a zero start the limiter never waits.
// On the present thread, where the limiter runs.
bool SetClockRate(int fps) {
    const uintptr_t clock = Clock();
    uint32_t ticks = 0;
    if (!clock || !Read(clock + kClockTicks, ticks)) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t start = now.QuadPart - static_cast<int64_t>(ticks) * g_qpcFrequency / fps;
    return Write(clock + kClockRate, static_cast<uint32_t>(fps)) && Write(clock + kClockStart, start);
}

// The engine setting its own rate (scene changes?): logged the first times.
void LogReapply(int rate) {
    static int count = 0;
    if (++count <= 20)
        log::Info("High FPS: the engine set its frame clock to {} FPS, set again{}", rate,
                  count == 20 ? " (not logged any more)" : "");
}

void LogClock(const char* when) {
    const uintptr_t clock = Clock();
    uint32_t rate = 0, ticks = 0;
    int64_t start = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (clock && Read(clock + kClockRate, rate) && Read(clock + kClockTicks, ticks) && Read(clock + kClockStart, start))
        log::Info("High FPS: frame clock {}: rate {}, ticks {}, {:.3f} s since its start ({} ticks due)", when, rate, ticks,
                  static_cast<double>(now.QuadPart - start) / g_qpcFrequency,
                  (now.QuadPart - start) * static_cast<int64_t>(rate) / g_qpcFrequency);
}

void Apply(int target) {
    const bool high = target > kGameFps;
    g_limit = target;
    LogClock("before");
    if (!SetClockRate(target)) {
        log::Warn("High FPS: the engine frame clock is not there yet");
        return;
    }
    LogClock("after");
    // Until the first frame is measured: the target's frame time.
    g_dtScale = static_cast<float>(kGameFps) / static_cast<float>(target);
    g_active = high;
    if (g_appliedTarget != target)
        log::Info("High FPS: frame clock at {} FPS{}", target, high ? ", time steps follow the real frame time" : "");
    g_appliedTarget = target;
}

// ---------------------------------------------------------------------------------------------------
// Finding things
// ---------------------------------------------------------------------------------------------------

bool FindAll() {
    g_image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto missing = [](const char* what) {
        log::Warn("High FPS: {} not found in this executable - FPS unlock unavailable", what);
        g_reason = "the game's frame pacing was not found in this executable (see log)";
        return false;
    };

    const auto clockHit = FindPattern(kClockPattern);
    if (!clockHit) return missing("frame clock");
    const uintptr_t getClock = Relative(*clockHit, 1, 5);
    if (*reinterpret_cast<const uint16_t*>(getClock) != 0x8B48 || *reinterpret_cast<const uint8_t*>(getClock + 2) != 0x05)
        return missing("frame clock accessor");
    g_clockGlobal = Relative(getClock, 3, 7);


    const auto moveMapHit = FindPattern(kMoveMapPattern);
    const auto moveMapVTables = rtti::FindVTables(".?AVMoveMapStep@NS_FRPG@@");
    if (!moveMapHit || moveMapVTables.empty() || Relative(*moveMapHit + 15, 3, 7) != moveMapVTables.front())
        return missing("MoveMapStep");
    const uintptr_t moveMapSteps = Relative(*moveMapHit + 4, 3, 7);

    // The limiter's wait: optional (without it the limit only comes back up by trying).
    if (const auto waitHit = FindPattern(kWaitPattern);
        waitHit && *reinterpret_cast<const uint16_t*>(*waitHit + kWaitFirstReturn - 6) == 0x15FF &&
        *reinterpret_cast<const uint16_t*>(*waitHit + kWaitLoopReturn - 6) == 0x15FF) {
        g_waitFirst = *waitHit + kWaitFirstReturn;
        g_waitLoop = *waitHit + kWaitLoopReturn;
        g_waitMeasured = hooks::PatchImport(GetModuleHandleW(nullptr), "kernel32.dll", "QueryPerformanceCounter", &hkQpc, &oQpc);
    }
    if (!g_waitMeasured) log::Warn("High FPS: the limiter's wait isn't measured - the limit comes back up by trying");

    const auto remoHit = FindPattern(kRemoPattern);
    if (!remoHit) return missing("cutscene steps");
    g_remoTable = Relative(*remoHit + 2, 3, 7);

    const auto fx = rtti::FindVTables(
        ".?AV?$frpgFxManagerBase@VFXHGManager@FX4HG@@V?$FrpgFxAdapterBase@V?$FXHGAdapterBase@VFXAdapter@FFX@@@FX4HG@@@"
        "NS_FRPG@@@NS_FRPG@@");
    if (fx.empty()) return missing("frpgFxManagerBase");

    const auto press = rtti::FindVTables(".?AVDLUserInputPressDetector@DLUI@@");
    const auto repeat = rtti::FindVTables(".?AVDLUserInputRepeatDetector@DLUI@@");
    if (press.empty() || repeat.empty()) return missing("DLUserInput detectors");

    const auto havokHit = FindPattern(kHavokPattern);
    if (!havokHit) return missing("Havok step call");
    g_havokCall = *havokHit + kHavokCallOffset;
    const auto menuHit = FindPattern(kMenuQueryPattern);
    if (!menuHit) return missing("menu action query");
    g_menuQuery = *menuHit;

    // The detectors' action list helpers: jmp -> Update: xor edx, edx; mov rcx, r9; call helper.
    for (size_t i = 0; i < g_detectors.size(); ++i) {
        Detector& d = g_detectors[i];
        uintptr_t fn = *reinterpret_cast<const uintptr_t*>((d.repeat ? repeat : press).front() + d.slot * 8);
        if (*reinterpret_cast<const uint8_t*>(fn) == 0xE9) fn = Relative(fn, 1, 5);
        const auto* code = reinterpret_cast<const uint8_t*>(fn);
        for (size_t k = 0; k + 10 <= 0x40 && !d.list; ++k)
            if (!memcmp(code + k, "\x33\xD2\x49\x8B\xC9\xE8", 6))
                d.list = reinterpret_cast<ActionList_t>(Relative(fn + k + 5, 1, 5));
        if (!d.list) return missing("DLUserInput detector action list");
    }

    // Hooks (data: vtables and step tables).
    const hooks::Batch batch;
    bool ok = HookSlot(moveMapSteps + kMoveMapUpdateStep * 8, reinterpret_cast<void*>(&hkMoveMapUpdate),
                       reinterpret_cast<void**>(&oMoveMapUpdate), "MoveMapStep update") &&
              HookSlot(fx.front() + kFxUpdateSlot * 8, reinterpret_cast<void*>(&hkFxUpdate),
                       reinterpret_cast<void**>(&oFxUpdate), "frpgFxManagerBase update");
    void* const kDetectorHooks[] = {reinterpret_cast<void*>(&hkDetector<0>), reinterpret_cast<void*>(&hkDetector<1>),
                                        reinterpret_cast<void*>(&hkDetector<2>), reinterpret_cast<void*>(&hkDetector<3>)};
    for (size_t i = 0; ok && i < g_detectors.size(); ++i) {
        Detector& d = g_detectors[i];
        ok = HookSlot((d.repeat ? repeat : press).front() + d.slot * 8, kDetectorHooks[i],
                      reinterpret_cast<void**>(&d.original), d.name);
    }
    if (!ok) {
        g_reason = "hooking the game's time steps failed (see log)";
        return false;
    }
    log::Info("High FPS: frame clock pointer at exe+0x{:x}, simulation step exe+0x{:x}, FX exe+0x{:x}, Havok call "
              "exe+0x{:x}, menu query exe+0x{:x}",
              Rva(g_clockGlobal), Rva(reinterpret_cast<uintptr_t>(oMoveMapUpdate)), Rva(reinterpret_cast<uintptr_t>(oFxUpdate)),
              Rva(g_havokCall), Rva(g_menuQuery));
    return true;
}

// The cutscene step table is filled by a static initializer: hooked once it is.
void TryHookRemo() {
    if (g_remoHooked || !g_remoTable) return;
    const uintptr_t slot = g_remoTable + kRemoUpdateStep * 8;
    const uintptr_t step = *reinterpret_cast<const uintptr_t*>(slot);
    if (!step) return;
    g_remoHooked = true;
    if (HookSlot(slot, reinterpret_cast<void*>(&hkRemoUpdate), reinterpret_cast<void**>(&oRemoUpdate), "cutscene update"))
        log::Info("High FPS: cutscene step exe+0x{:x}", Rva(step));
}

// Frames per second over one-second windows: a limiter that doesn't hold the target (the frame clock
// isn't what this module thinks) puts the game back to 60 rather than letting it run unbounded.
bool g_limiterFailed = false;
int64_t g_windowStart = 0;
int g_windowFrames = 0, g_overWindows = 0;

// The engine skips rendering a frame when it falls behind its clock: with the GPU as the limit it runs
// two simulation updates for some presented frames and one for others, and the image judders. The
// clock's rate goes down to what is presented (no skipped frames), then back up by steps while the
// frames keep up, towards the target. Without skipping, one update per presented frame.
// Back up: from the limiter's wait, the capacity left unused (frames ready early: the GPU was freed by a
// lower resolution, DLSS 5 off, another program closed...): fps / (1 - wait share), straight to 95 % of
// it. Without the wait measured, by steps.
void AdaptLimit(int target, double fps, double updates, double waitShare) {
    static int calm = 0;           // seconds without skipped frames
    static int headroom = 0;       // seconds in a row with the limiter waiting
    static double ceiling = 1e9;   // presented FPS the last time frames were skipped
    int limit = g_limit.load();
    const bool skipping = updates > fps * 1.04 + 1.0;
    if (skipping) {
        calm = headroom = 0;
        ceiling = fps;
        const int lower = std::max(kGameFps, static_cast<int>(fps * 0.97));
        if (lower >= limit) return;
        limit = lower;
    } else if (waitShare >= 0.0) {
        if (limit >= target || waitShare < 0.06) {
            headroom = 0;
            return;
        }
        if (++headroom < 2) return;
        headroom = 0;
        ceiling = 1e9;
        const double capacity = fps / std::max(0.05, 1.0 - waitShare);
        limit = std::min(target, std::max(limit + 2, static_cast<int>(capacity * 0.95)));
    } else {
        ++calm;
        if (limit >= target) return;
        // Up to just below where frames were last skipped every 3 s; past it, a try every 30 s.
        const bool belowCeiling = limit < ceiling * 0.97;
        if (calm < (belowCeiling ? 3 : 30)) return;
        calm = 0;
        limit = std::min(target, limit + std::max(2, limit / 30));
        if (!belowCeiling) ceiling = 1e9;
    }
    static int logged = 0;
    if (++logged <= 30)
        log::Info("High FPS: limit {} -> {} ({:.1f} frames presented/s, {:.1f} simulation updates/s, limiter waiting "
                  "{:.0f} %){}", g_limit.load(), limit, fps, updates, std::max(0.0, waitShare) * 100,
                  logged == 30 ? " (not logged any more)" : "");
    g_limit = limit;
    SetClockRate(limit);
}

void WatchLimiter(int target) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ++g_windowFrames;
    if (!g_windowStart) g_windowStart = now.QuadPart;
    const double seconds = static_cast<double>(now.QuadPart - g_windowStart) / g_qpcFrequency;
    if (seconds < 1.0) return;
    const double fps = g_windowFrames / seconds;
    static int windows = 0;
    static uint64_t lastSteps = 0;
    const uint64_t steps = g_simulationSteps.load();
    const double updates = (steps - lastSteps) / seconds;
    static int64_t lastWait = 0;
    const int64_t wait = g_waitTotal.load();
    const double waitShare = g_waitMeasured ? static_cast<double>(wait - lastWait) / (seconds * g_qpcFrequency) : -1.0;
    lastWait = wait;
    if (++windows % 30 == 0 && g_active.load())
        log::Info("High FPS: {:.1f} frames presented/s, {:.1f} simulation updates/s, limit {}, limiter waiting {:.0f} % "
                  "(step x{:.3f})", fps, updates, g_limit.load(), std::max(0.0, waitShare) * 100, g_dtScale.load());
    lastSteps = steps;
    g_windowStart = now.QuadPart;
    g_windowFrames = 0;
    if (g_active.load() && updates > 0) AdaptLimit(target, fps, updates, waitShare);
    g_overWindows = fps > g_limit.load() * 1.5 + 10 ? g_overWindows + 1 : 0;
    if (g_overWindows < 2) return;
    log::Error("High FPS: {:.0f} FPS with the limit at {} - the frame limiter isn't holding, back to 60 FPS", fps, target);
    LogClock("not holding");
    g_limiterFailed = true;
    g_target = kGameFps;
}

}  // namespace

void Install(int targetFps) {
    g_target = std::clamp(targetFps, kGameFps, kMaxFps);
    g_installTick = GetTickCount64();
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_qpcFrequency = frequency.QuadPart;
    if (!FindAll()) return;
    TryHookRemo();
    if (!InstallMouse()) log::Warn("High FPS: DirectInput mouse hooks failed - the camera may turn slower with a mouse");
    g_available = true;
    g_reason = "";
    log::Info("High FPS: ready, target {} FPS", g_target.load());
}

void OnFrame() {
    if (!g_available) return;
    TryHookRemo();
    const int target = g_target.load();
    if (target <= kGameFps && !g_appliedTarget) return;  // never touched: the game is as it was
    if (!Clock()) return;                                 // not created yet
    // Once per target, and again if the engine set its own rate. Only then: moving the clock's start
    // every frame would release the limiter every frame.
    if (g_appliedTarget != target) {
        Apply(target);
    } else if (const int rate = ClockRate(); rate >= 0 && rate != g_limit.load()) {
        LogReapply(rate);
        SetClockRate(g_limit.load());
    }
    if (!g_limiterFailed) WatchLimiter(target);
}

void SetTarget(int fps) {
    if (g_limiterFailed) return;
    fps = std::clamp(fps, kGameFps, kMaxFps);
    if (g_target.exchange(fps) != fps) log::Info("High FPS: target {} FPS", fps);
}

Status GetStatus() {
    Status s;
    s.available = g_available;
    s.active = g_active.load();
    s.target = g_target.load();
    s.limit = g_limit.load();
    if (!g_available)
        s.reason = g_reason;
    else if (g_limiterFailed)
        s.reason = "the frame limiter didn't hold the limit: back to 60 FPS (see log)";
    else if (s.target > kGameFps && !s.active)
        s.reason = "waiting for the engine's frame clock";
    return s;
}

}  // namespace dsru::game::high_fps
