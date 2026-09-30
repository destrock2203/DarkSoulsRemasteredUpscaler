#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>

// The D3D11 -> D3D12 bridge. Dark Souls Remastered renders with D3D11; the upscalers (FSR 3.1 / 4,
// XeSS on any GPU, DLSS through Streamline) and every frame generation backend need D3D12. The bridge
// creates a D3D12 device on the game's adapter and textures that both devices see (created on D3D12
// with a shared heap, opened on D3D11), and orders the two queues with two shared fences:
//     D3D11 immediate context: ... copies into shared textures -> Signal(toD3D12, n)
//     D3D12 queue:             Wait(toD3D12, n) -> upscaler / frame generation -> Signal(toD3D11, m)
//     D3D11 immediate context: Wait(toD3D11, m) -> ... reads the results
// Everything waits on the GPU; the CPU only blocks when a command allocator is reused while still in
// flight. Called from the game's immediate-context thread (which is also its present thread).
namespace dsru::render::bridge {

// From the D3D11CreateDevice hook (the game's device). Creates the D3D12 side on the same adapter.
// `streamlineQueue`: create the queue through Streamline's proxy device (DLSS frame generation).
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* immediate, bool streamlineQueue);
bool Ready();

ID3D11Device5* Device11();
ID3D11DeviceContext4* Context11();
ID3D12Device* Device12();
ID3D12CommandQueue* Queue();
IDXGIFactory4* Factory();
LUID AdapterLuid();

// A texture both devices use. D3D12 keeps it in D3D12_RESOURCE_STATE_COMMON between uses.
struct SharedTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    explicit operator bool() const { return d3d11 != nullptr; }
    void Reset() { *this = {}; }
};

enum SharedUsage : uint32_t {
    kShaderResource = 1u << 0,
    kRenderTarget = 1u << 1,
    kUnorderedAccess = 1u << 2,
};

// (Re)creates `texture` unless it already has this size / format / usage. D3D11 views are created for
// the requested usages (typed `viewFormat`, or `format` when UNKNOWN).
bool EnsureShared(SharedTexture& texture, UINT width, UINT height, DXGI_FORMAT format, uint32_t usage,
                  const wchar_t* name, DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN);

// --- Ordering ---
// Immediate context: signal the D3D12 side (and flush, so the GPU sees it). Returns the value.
uint64_t SignalFrom11();
// D3D12 queue: wait for a value signaled by SignalFrom11.
void WaitOn12(uint64_t value);
// D3D12 queue: signal the D3D11 side. Returns the value.
uint64_t SignalFrom12();
// Immediate context: wait (GPU) for a value signaled by SignalFrom12.
void WaitOn11(uint64_t value);
// Both queues idle (resize, shutdown).
void WaitIdle();

// --- D3D12 command lists (ring of allocators) ---
ID3D12GraphicsCommandList* BeginList();
// Closes and executes the list from BeginList.
void Submit(ID3D12GraphicsCommandList* list);

// Transition barrier helper.
D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

// On the thread creating a D3D12 swapchain for the presenter: the DXGI hooks let those calls through.
bool CreatingOwnObjects();
struct OwnObjectsScope {
    OwnObjectsScope();
    ~OwnObjectsScope();
};

void Shutdown();

}  // namespace dsru::render::bridge
