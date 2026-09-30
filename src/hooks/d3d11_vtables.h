#pragma once

#include <cstddef>

// Vtable slots of the D3D11 / DXGI methods the mod and the probe hook. Checked against the SDK's
// C interface declarations at compile time (vtable_check.c).
namespace dsru::hooks::vt {

// ID3D11Device
inline constexpr size_t kDevice_CreateBuffer = 3;
inline constexpr size_t kDevice_CreateTexture2D = 5;
inline constexpr size_t kDevice_CreateShaderResourceView = 7;
inline constexpr size_t kDevice_CreateUnorderedAccessView = 8;
inline constexpr size_t kDevice_CreateRenderTargetView = 9;
inline constexpr size_t kDevice_CreateDepthStencilView = 10;
inline constexpr size_t kDevice_CreateVertexShader = 12;
inline constexpr size_t kDevice_CreatePixelShader = 15;
inline constexpr size_t kDevice_CreateComputeShader = 18;
inline constexpr size_t kDevice_CreateSamplerState = 23;
inline constexpr size_t kDevice_CreateDeferredContext = 27;

// ID3D11DeviceContext
inline constexpr size_t kContext_VSSetConstantBuffers = 7;
inline constexpr size_t kContext_PSSetShaderResources = 8;
inline constexpr size_t kContext_PSSetShader = 9;
inline constexpr size_t kContext_PSSetSamplers = 10;
inline constexpr size_t kContext_VSSetShader = 11;
inline constexpr size_t kContext_DrawIndexed = 12;
inline constexpr size_t kContext_Draw = 13;
inline constexpr size_t kContext_Map = 14;
inline constexpr size_t kContext_Unmap = 15;
inline constexpr size_t kContext_PSSetConstantBuffers = 16;
inline constexpr size_t kContext_DrawIndexedInstanced = 20;
inline constexpr size_t kContext_DrawInstanced = 21;
inline constexpr size_t kContext_OMSetRenderTargets = 33;
inline constexpr size_t kContext_OMSetRenderTargetsAndUnorderedAccessViews = 34;
inline constexpr size_t kContext_Dispatch = 41;
inline constexpr size_t kContext_RSSetViewports = 44;
inline constexpr size_t kContext_RSSetScissorRects = 45;
inline constexpr size_t kContext_CopySubresourceRegion = 46;
inline constexpr size_t kContext_CopyResource = 47;
inline constexpr size_t kContext_UpdateSubresource = 48;
inline constexpr size_t kContext_ClearRenderTargetView = 50;
inline constexpr size_t kContext_ClearDepthStencilView = 53;
inline constexpr size_t kContext_ResolveSubresource = 57;
inline constexpr size_t kContext_ExecuteCommandList = 58;
inline constexpr size_t kContext_CSSetShaderResources = 67;
inline constexpr size_t kContext_CSSetUnorderedAccessViews = 68;
inline constexpr size_t kContext_CSSetShader = 69;
inline constexpr size_t kContext_CSSetConstantBuffers = 71;
inline constexpr size_t kContext_ClearState = 110;
inline constexpr size_t kContext_FinishCommandList = 114;

// ID3D11DeviceContext1 (constant buffers bound with offsets into a larger buffer)
inline constexpr size_t kContext1_UpdateSubresource1 = 116;
inline constexpr size_t kContext1_VSSetConstantBuffers1 = 119;
inline constexpr size_t kContext1_PSSetConstantBuffers1 = 123;

// IDXGIFactory / IDXGIFactory2
inline constexpr size_t kFactory_CreateSwapChain = 10;
inline constexpr size_t kFactory2_CreateSwapChainForHwnd = 15;

// IDXGISwapChain
inline constexpr size_t kSwapChain_Present = 8;
inline constexpr size_t kSwapChain_GetBuffer = 9;
inline constexpr size_t kSwapChain_SetFullscreenState = 10;
inline constexpr size_t kSwapChain_ResizeBuffers = 13;
inline constexpr size_t kSwapChain_ResizeTarget = 14;

// IDXGIOutput
inline constexpr size_t kOutput_WaitForVBlank = 10;

}  // namespace dsru::hooks::vt
