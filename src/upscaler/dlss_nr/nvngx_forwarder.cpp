// dsru_nvngx.dll: relays the four NGX entry points of nvngx_dlssnr.dll (DLSS 5, 3D-guided neural
// rendering) for upscaler/dlss_nr.cpp.
//
// nvngx_dlssnr.dll serves a call only when the module the call returns to has "nvngx.dll" in its
// path (it refuses dxgi.dll with NVSDK_NGX_Result_FAIL_PlatformError). This module's name carries
// it, so the calls are made from here. Each wrapper must keep a real frame: a tail call
// (`return fn(...)` compiled to `jmp`) would leave the caller's return address on the stack.
//
// The feature library itself is loaded by dxgi.dll after its NVIDIA signature check; this module
// only resolves the entry points of that already loaded library.
#include <Windows.h>
#include <d3d12.h>

#include <nvsdk_ngx.h>

namespace {

using InitExt_t = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                                 const NVSDK_NGX_Parameter*);
using Create_t = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using Evaluate_t = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                  const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using Release_t = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

InitExt_t g_initExt = nullptr;
Create_t g_create = nullptr;
Evaluate_t g_evaluate = nullptr;
Release_t g_release = nullptr;

// Written after every call: keeps each wrapper from compiling to a tail call.
volatile LONG g_lastResult = 0;

NVSDK_NGX_Result Keep(NVSDK_NGX_Result result) {
    InterlockedExchange(&g_lastResult, static_cast<LONG>(result));
    return result;
}

}  // namespace

extern "C" {

// Resolves the entry points of the loaded feature library. 0 on success, -1 if one is missing.
__declspec(dllexport) int DsruNrBind(HMODULE library) {
    g_initExt = reinterpret_cast<InitExt_t>(GetProcAddress(library, "NVSDK_NGX_D3D12_Init_Ext"));
    g_create = reinterpret_cast<Create_t>(GetProcAddress(library, "NVSDK_NGX_D3D12_CreateFeature"));
    g_evaluate = reinterpret_cast<Evaluate_t>(GetProcAddress(library, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_release = reinterpret_cast<Release_t>(GetProcAddress(library, "NVSDK_NGX_D3D12_ReleaseFeature"));
    return g_initExt && g_create && g_evaluate && g_release ? 0 : -1;
}

__declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DsruNrInitExt(unsigned long long appId, const wchar_t* dataPath,
                                                                  ID3D12Device* device, NVSDK_NGX_Version version,
                                                                  const NVSDK_NGX_Parameter* params) {
    return Keep(g_initExt(appId, dataPath, device, version, params));
}

__declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DsruNrCreate(ID3D12GraphicsCommandList* list,
                                                                 NVSDK_NGX_Feature feature,
                                                                 const NVSDK_NGX_Parameter* params,
                                                                 NVSDK_NGX_Handle** handle) {
    return Keep(g_create(list, feature, params, handle));
}

__declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DsruNrEvaluate(ID3D12GraphicsCommandList* list,
                                                                   const NVSDK_NGX_Handle* handle,
                                                                   const NVSDK_NGX_Parameter* params) {
    return Keep(g_evaluate(list, handle, params, nullptr));
}

__declspec(dllexport) __declspec(noinline) NVSDK_NGX_Result DsruNrRelease(NVSDK_NGX_Handle* handle) {
    return Keep(g_release(handle));
}

}  // extern "C"
