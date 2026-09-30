#include "core/crash_handler.h"

#include "core/log.h"
#include "core/paths.h"

#include <Windows.h>

#include <atomic>
#include <string>

namespace dsru::crash {

namespace {

constexpr DWORD kCppException = 0xE06D7363;
constexpr int kMaxReports = 32;
constexpr int kMaxFrames = 32;

std::atomic<int> g_reports{0};
thread_local bool t_inside = false;
thread_local int t_expectFaults = 0;

bool IsInteresting(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case kCppException: return true;
        default: return false;
    }
}

std::string Describe(DWORD64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return std::format("0x{:X}", address);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    std::string narrow;
    for (const wchar_t* c = name; *c; ++c) narrow.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    // We are dxgi.dll too; without the tag our frames and System32 dxgi.dll's look the same.
    const char* tag = module == paths::SelfModule() ? " [mod]" : "";
    return std::format("{}{}+0x{:X}", narrow, tag, address - reinterpret_cast<DWORD64>(module));
}

// Plain C-style function so it can use SEH: a corrupted stack must not fault the handler.
int WalkStack(const CONTEXT* start, DWORD64* frames, int maxFrames) {
    CONTEXT context = *start;
    int count = 0;
    __try {
        // A call through a null (or garbage) function pointer: nothing to unwind at rip, but the
        // call just pushed the return address, which leads back to the caller.
        HMODULE owner = nullptr;
        if (count < maxFrames &&
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(context.Rip), &owner)) {
            frames[count++] = context.Rip;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
        while (count < maxFrames && context.Rip) {
            frames[count++] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (function) {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                 &establisher, nullptr);
            } else {
                // Leaf function: the return address is on top of the stack.
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

bool IsOurs(DWORD64 address) {
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) &&
           module == paths::SelfModule();
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (!IsInteresting(record->ExceptionCode) || t_inside) return EXCEPTION_CONTINUE_SEARCH;
    if (t_expectFaults && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    t_inside = true;
    const CONTEXT& ctx = *info->ContextRecord;

    DWORD64 frames[kMaxFrames];
    const int count = record->ExceptionCode != EXCEPTION_STACK_OVERFLOW ? WalkStack(&ctx, frames, kMaxFrames) : 0;

    // System DLLs (DXGI during mode switches, for one) throw and catch C++ exceptions as part of
    // normal operation. Only report the ones thrown from our own code.
    if (record->ExceptionCode == kCppException) {
        bool ours = false;
        for (int i = 0; i < count && i < 4 && !ours; ++i) ours = IsOurs(frames[i]);
        if (!ours) {
            log::Debug("C++ exception thrown outside the mod at {} (normal for DXGI)",
                       count > 2 ? Describe(frames[2]) : Describe(reinterpret_cast<DWORD64>(record->ExceptionAddress)));
            t_inside = false;
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    if (g_reports.fetch_add(1) >= kMaxReports) {
        t_inside = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    log::Error("Exception 0x{:08X} at {} (first chance: the game may still handle it)", record->ExceptionCode,
               Describe(reinterpret_cast<DWORD64>(record->ExceptionAddress)));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        log::Error("  {} of address 0x{:X}", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                   record->ExceptionInformation[1]);
    }
    log::Error("  rax={:X} rbx={:X} rcx={:X} rdx={:X} rsi={:X} rdi={:X}", ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx,
               ctx.Rsi, ctx.Rdi);
    log::Error("  r8={:X} r9={:X} rsp={:X} rip={:X}", ctx.R8, ctx.R9, ctx.Rsp, ctx.Rip);

    for (int i = 0; i < count; ++i) log::Error("  #{:<2} {}", i, Describe(frames[i]));

    t_inside = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

ExpectFaults::ExpectFaults() { ++t_expectFaults; }
ExpectFaults::~ExpectFaults() { --t_expectFaults; }

void Install() {
    if (!AddVectoredExceptionHandler(1, Handler)) log::Warn("Failed to install the crash logger");
}

}  // namespace dsru::crash
