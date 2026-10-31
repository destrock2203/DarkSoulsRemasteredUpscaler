#include "render/sharpen.h"

#include "core/log.h"
#include "shaders/sharpen_cs.h"

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace dsru::render::sharpen {

namespace {

// The source SRV and destination UAV are written on every use into the next pair of a ring: a pair
// is only overwritten once the frames that used it are long done (never more than a few in flight),
// and a recreated texture (resize) never leaves a stale descriptor behind.
constexpr UINT kSlots = 16;

bool g_initialized = false;
bool g_failed = false;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pipeline;
ComPtr<ID3D12DescriptorHeap> g_heap;  // shader visible, kSlots x (SRV, UAV)
UINT g_increment = 0;
UINT g_next = 0;

using Serialize_t = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                     ID3DBlob**);

bool Initialize(ID3D12Device* device) {
    const D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                              {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 6};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};

    const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    const auto serialize =
        reinterpret_cast<Serialize_t>(d3d12 ? GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr);
    ComPtr<ID3DBlob> blob, errors;
    if (!serialize || FAILED(serialize(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_rootSignature)))) {
        log::Error("Sharpen: root signature creation failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.CS = {g_sharpen_cs, sizeof(g_sharpen_cs)};
    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pipeline)))) {
        log::Error("Sharpen: pipeline creation failed");
        return false;
    }
    const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 2,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_heap)))) {
        log::Error("Sharpen: descriptor heap creation failed");
        return false;
    }
    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    log::Info("Sharpen: AMD RCAS and Laplacian ready");
    return true;
}

bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
            uint32_t width, uint32_t height, uint32_t mode, float strength, float rangeMin, float rangeMax) {
    if (g_failed || !device) return false;
    if (!g_initialized) {
        g_initialized = true;
        if (!Initialize(device)) {
            g_failed = true;
            return false;
        }
    }
    const UINT slot = (g_next++ % kSlots) * 2;
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu{g_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(slot) * g_increment};
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu{g_heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(slot) * g_increment};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(src, &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(dst, nullptr, &uav, {cpu.ptr + g_increment});

    uint32_t constants[6] = {width, height, 0, mode, 0, 0};
    std::memcpy(&constants[2], &strength, sizeof(float));
    std::memcpy(&constants[4], &rangeMin, sizeof(float));
    std::memcpy(&constants[5], &rangeMax, sizeof(float));
    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_rootSignature.Get());
    list->SetPipelineState(g_pipeline.Get());
    list->SetComputeRoot32BitConstants(0, 6, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

}  // namespace

bool Rcas(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
          uint32_t width, uint32_t height, float sharpness) {
    // FSR's mapping: sharpness 1 -> 0 stops (strongest), 0 -> 2 stops; FsrRcasCon: exp2(-stops).
    const float con = std::exp2(-(2.0f - 2.0f * std::clamp(sharpness, 0.0f, 1.0f)));
    return Record(device, list, src, dst, width, height, 0, con, 0.0f, 0.0f);
}

bool Laplacian(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
               uint32_t width, uint32_t height, float sharpness) {
    // Elden Ring clamps the log-space Laplacian of linear color to about +-1; on gamma-encoded color
    // (~2.2) the same edges give 2.2 times less.
    constexpr float kRange = 1.0f / 2.2f;
    return Record(device, list, src, dst, width, height, 1, std::clamp(sharpness, 0.0f, 1.0f), -kRange, kRange);
}

}  // namespace dsru::render::sharpen
