#include "render/bridge.h"

#include "core/log.h"
#include "upscaler/streamline/streamline.h"
#include "upscaler/upscaler.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace dsru::render::bridge {

namespace {

ComPtr<ID3D11Device5> g_device11;
ComPtr<ID3D11DeviceContext4> g_context11;
ComPtr<ID3D12Device> g_device12;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<IDXGIFactory4> g_factory;
LUID g_luid{};
std::atomic<bool> g_ready{false};

// toD3D12: signaled by D3D11, waited on by D3D12. toD3D11: the reverse. Both created on D3D12 and
// opened on D3D11. listFence: D3D12 only, completion of our command lists.
ComPtr<ID3D12Fence> g_toD3D12, g_toD3D11, g_listFence;
ComPtr<ID3D11Fence> g_toD3D12On11, g_toD3D11On11;
uint64_t g_toD3D12Value = 0, g_toD3D11Value = 0, g_listValue = 0;
HANDLE g_event = nullptr;
std::recursive_mutex g_mutex;

struct ListSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fence = 0;
};
std::array<ListSlot, 8> g_lists;
size_t g_nextList = 0;
ListSlot* g_openList = nullptr;

thread_local int t_ownObjects = 0;

void WaitCpu(ID3D12Fence* fence, uint64_t value) {
    if (fence->GetCompletedValue() >= value) return;
    fence->SetEventOnCompletion(value, g_event);
    WaitForSingleObject(g_event, 5000);
}

ComPtr<ID3D11Fence> OpenOn11(ID3D12Fence* fence, const char* name) {
    HANDLE handle = nullptr;
    ComPtr<ID3D11Fence> opened;
    if (FAILED(g_device12->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &handle))) {
        log::Error("Bridge: CreateSharedHandle({}) failed", name);
        return nullptr;
    }
    const HRESULT hr = g_device11->OpenSharedFence(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    if (FAILED(hr)) log::Error("Bridge: OpenSharedFence({}) failed: 0x{:08x}", name, static_cast<unsigned>(hr));
    return opened;
}

}  // namespace

OwnObjectsScope::OwnObjectsScope() { ++t_ownObjects; }
OwnObjectsScope::~OwnObjectsScope() { --t_ownObjects; }
bool CreatingOwnObjects() { return t_ownObjects > 0; }

bool Initialize(ID3D11Device* device, ID3D11DeviceContext* immediate, bool streamlineQueue) {
    std::lock_guard lock(g_mutex);
    if (g_ready) return true;
    OwnObjectsScope scope;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&g_device11))) ||
        FAILED(immediate->QueryInterface(IID_PPV_ARGS(&g_context11)))) {
        log::Error("Bridge: the game's device lacks ID3D11Device5 / ID3D11DeviceContext4 (Windows 10 1703+ needed)");
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    g_luid = adapterDesc.AdapterLuid;

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory)))) {
        log::Error("Bridge: CreateDXGIFactory2 failed");
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter1;
    if (FAILED(g_factory->EnumAdapterByLuid(g_luid, IID_PPV_ARGS(&adapter1)))) {
        log::Error("Bridge: the game's adapter was not found by LUID");
        return false;
    }

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (!create) {
        log::Error("Bridge: d3d12.dll unavailable");
        return false;
    }
    // Streamline must be initialized before the D3D12 device exists.
    streamline::Init();
    HRESULT hr = create(adapter1.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device12));
    if (FAILED(hr)) hr = create(adapter1.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device12));
    if (FAILED(hr)) {
        log::Error("Bridge: D3D12CreateDevice failed: 0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }
    g_device12->SetName(L"DSRUpscaler bridge device");
    streamline::OnDeviceCreated(g_device12.Get());
    upscaler::OnDeviceCreated(g_device12.Get());

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool handled = false;
    if (streamlineQueue)
        hr = streamline::CreateCommandQueue(g_device12.Get(), &queueDesc, IID_PPV_ARGS(&g_queue), handled);
    if (!handled) hr = g_device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue));
    if (FAILED(hr) || !g_queue) {
        log::Error("Bridge: CreateCommandQueue failed: 0x{:08x}", static_cast<unsigned>(hr));
        return false;
    }
    g_queue->SetName(L"DSRUpscaler bridge queue");

    if (FAILED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_toD3D12))) ||
        FAILED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_toD3D11))) ||
        FAILED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_listFence)))) {
        log::Error("Bridge: CreateFence failed");
        return false;
    }
    g_toD3D12On11 = OpenOn11(g_toD3D12.Get(), "toD3D12");
    g_toD3D11On11 = OpenOn11(g_toD3D11.Get(), "toD3D11");
    if (!g_toD3D12On11 || !g_toD3D11On11) return false;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (ListSlot& slot : g_lists) {
        if (FAILED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&slot.list)))) {
            log::Error("Bridge: command list creation failed");
            return false;
        }
        slot.list->Close();
    }

    std::string name;
    for (const wchar_t* c = adapterDesc.Description; *c; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    log::Info("Bridge: D3D12 device on '{}' (feature level {:x}){}", name,
              static_cast<int>([] {
                  D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};
                  static const D3D_FEATURE_LEVEL list[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                                           D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                                           D3D_FEATURE_LEVEL_11_0};
                  levels.NumFeatureLevels = static_cast<UINT>(std::size(list));
                  levels.pFeatureLevelsRequested = list;
                  g_device12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels));
                  return levels.MaxSupportedFeatureLevel;
              }()),
              handled ? ", queue through Streamline" : "");
    g_ready = true;
    return true;
}

bool Ready() { return g_ready.load(); }
ID3D11Device5* Device11() { return g_device11.Get(); }
ID3D11DeviceContext4* Context11() { return g_context11.Get(); }
ID3D12Device* Device12() { return g_device12.Get(); }
ID3D12CommandQueue* Queue() { return g_queue.Get(); }
IDXGIFactory4* Factory() { return g_factory.Get(); }
LUID AdapterLuid() { return g_luid; }

bool EnsureShared(SharedTexture& t, UINT width, UINT height, DXGI_FORMAT format, uint32_t usage, const wchar_t* name,
                  DXGI_FORMAT viewFormat) {
    if (!g_ready || !width || !height) return false;
    if (t && t.width == width && t.height == height && t.format == format) return true;
    t.Reset();
    std::lock_guard lock(g_mutex);

    // Created on D3D11 and opened on D3D12: the other way round, D3D11 refuses most formats / flags
    // (E_INVALIDARG for R32F, RG16F, R11G11B10, anything with UAV on an RTX 5090, driver 610.47). D3D12
    // sees these with ALLOW_SIMULTANEOUS_ACCESS; they are kept in the COMMON state between uses.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (usage & kRenderTarget) desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (usage & kUnorderedAccess) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = g_device11->CreateTexture2D(&desc, nullptr, &t.d3d11);
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIResource1> resource;
        hr = t.d3d11.As(&resource);
        if (SUCCEEDED(hr))
            hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                              &handle);
    }
    if (SUCCEEDED(hr)) {
        hr = g_device12->OpenSharedHandle(handle, IID_PPV_ARGS(&t.d3d12));
        CloseHandle(handle);
    }
    if (FAILED(hr)) {
        log::Error("Bridge: shared texture {}x{} format {} failed: 0x{:08x}", width, height, static_cast<int>(format),
                   static_cast<unsigned>(hr));
        t.Reset();
        return false;
    }
    if (name) t.d3d12->SetName(name);
    const DXGI_FORMAT typed = viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : format;
    if (usage & kShaderResource) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = typed;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_device11->CreateShaderResourceView(t.d3d11.Get(), &srv, &t.srv);
    }
    if (usage & kRenderTarget) {
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = typed;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        g_device11->CreateRenderTargetView(t.d3d11.Get(), &rtv, &t.rtv);
    }
    if (usage & kUnorderedAccess) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = typed;
        uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        g_device11->CreateUnorderedAccessView(t.d3d11.Get(), &uav, &t.uav);
    }
    t.width = width;
    t.height = height;
    t.format = format;
    return true;
}

uint64_t SignalFrom11() {
    std::lock_guard lock(g_mutex);
    const uint64_t value = ++g_toD3D12Value;
    g_context11->Signal(g_toD3D12On11.Get(), value);
    g_context11->Flush();
    return value;
}

void WaitOn12(uint64_t value) {
    std::lock_guard lock(g_mutex);
    g_queue->Wait(g_toD3D12.Get(), value);
}

uint64_t SignalFrom12() {
    std::lock_guard lock(g_mutex);
    const uint64_t value = ++g_toD3D11Value;
    g_queue->Signal(g_toD3D11.Get(), value);
    return value;
}

void WaitOn11(uint64_t value) {
    std::lock_guard lock(g_mutex);
    g_context11->Wait(g_toD3D11On11.Get(), value);
}

void WaitIdle() {
    std::lock_guard lock(g_mutex);
    if (!g_ready) return;
    const uint64_t from11 = SignalFrom11();
    WaitCpu(g_toD3D12.Get(), from11);
    const uint64_t list = ++g_listValue;
    g_queue->Signal(g_listFence.Get(), list);
    WaitCpu(g_listFence.Get(), list);
}

ID3D12GraphicsCommandList* BeginList() {
    std::unique_lock lock(g_mutex);
    ListSlot& slot = g_lists[g_nextList];
    g_nextList = (g_nextList + 1) % g_lists.size();
    WaitCpu(g_listFence.Get(), slot.fence);
    slot.allocator->Reset();
    slot.list->Reset(slot.allocator.Get(), nullptr);
    g_openList = &slot;
    lock.release();  // held until Submit (recursive mutex)
    return slot.list.Get();
}

void Submit(ID3D12GraphicsCommandList* list) {
    std::unique_lock lock(g_mutex, std::adopt_lock);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    g_queue->ExecuteCommandLists(1, lists);
    if (g_openList) g_openList->fence = ++g_listValue;
    g_queue->Signal(g_listFence.Get(), g_listValue);
    g_openList = nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void Shutdown() {
    if (!g_ready) return;
    WaitIdle();
    g_ready = false;
}

}  // namespace dsru::render::bridge
