/* Compile-time check of hooks/d3d11_vtables.h against the SDK: the C declarations of the COM
 * interfaces list the methods in vtable order, so a slot index is the method's offset in the
 * Vtbl struct divided by the pointer size. Nothing here ends up in the binary. */
#define COBJMACROS
#define CINTERFACE
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stddef.h>

#define SLOT(vtbl, method) (offsetof(vtbl, method) / sizeof(void*))
#define CHECK(vtbl, method, index) _Static_assert(SLOT(vtbl, method) == (index), #vtbl "::" #method)

CHECK(ID3D11DeviceVtbl, CreateBuffer, 3);
CHECK(ID3D11DeviceVtbl, CreateTexture2D, 5);
CHECK(ID3D11DeviceVtbl, CreateShaderResourceView, 7);
CHECK(ID3D11DeviceVtbl, CreateUnorderedAccessView, 8);
CHECK(ID3D11DeviceVtbl, CreateRenderTargetView, 9);
CHECK(ID3D11DeviceVtbl, CreateDepthStencilView, 10);
CHECK(ID3D11DeviceVtbl, CreateVertexShader, 12);
CHECK(ID3D11DeviceVtbl, CreatePixelShader, 15);
CHECK(ID3D11DeviceVtbl, CreateComputeShader, 18);
CHECK(ID3D11DeviceVtbl, CreateSamplerState, 23);
CHECK(ID3D11DeviceVtbl, CreateDeferredContext, 27);

CHECK(ID3D11DeviceContextVtbl, VSSetConstantBuffers, 7);
CHECK(ID3D11DeviceContextVtbl, PSSetShaderResources, 8);
CHECK(ID3D11DeviceContextVtbl, PSSetShader, 9);
CHECK(ID3D11DeviceContextVtbl, PSSetSamplers, 10);
CHECK(ID3D11DeviceContextVtbl, VSSetShader, 11);
CHECK(ID3D11DeviceContextVtbl, DrawIndexed, 12);
CHECK(ID3D11DeviceContextVtbl, Draw, 13);
CHECK(ID3D11DeviceContextVtbl, Map, 14);
CHECK(ID3D11DeviceContextVtbl, Unmap, 15);
CHECK(ID3D11DeviceContextVtbl, PSSetConstantBuffers, 16);
CHECK(ID3D11DeviceContextVtbl, DrawIndexedInstanced, 20);
CHECK(ID3D11DeviceContextVtbl, DrawInstanced, 21);
CHECK(ID3D11DeviceContextVtbl, OMSetRenderTargets, 33);
CHECK(ID3D11DeviceContextVtbl, OMSetRenderTargetsAndUnorderedAccessViews, 34);
CHECK(ID3D11DeviceContextVtbl, Dispatch, 41);
CHECK(ID3D11DeviceContextVtbl, RSSetViewports, 44);
CHECK(ID3D11DeviceContextVtbl, RSSetScissorRects, 45);
CHECK(ID3D11DeviceContextVtbl, CopySubresourceRegion, 46);
CHECK(ID3D11DeviceContextVtbl, CopyResource, 47);
CHECK(ID3D11DeviceContextVtbl, UpdateSubresource, 48);
CHECK(ID3D11DeviceContextVtbl, ClearRenderTargetView, 50);
CHECK(ID3D11DeviceContextVtbl, ClearDepthStencilView, 53);
CHECK(ID3D11DeviceContextVtbl, ResolveSubresource, 57);
CHECK(ID3D11DeviceContextVtbl, ExecuteCommandList, 58);
CHECK(ID3D11DeviceContextVtbl, CSSetShaderResources, 67);
CHECK(ID3D11DeviceContextVtbl, CSSetUnorderedAccessViews, 68);
CHECK(ID3D11DeviceContextVtbl, CSSetShader, 69);
CHECK(ID3D11DeviceContextVtbl, CSSetConstantBuffers, 71);
CHECK(ID3D11DeviceContextVtbl, ClearState, 110);
CHECK(ID3D11DeviceContextVtbl, FinishCommandList, 114);
CHECK(ID3D11DeviceContext1Vtbl, UpdateSubresource1, 116);
CHECK(ID3D11DeviceContext1Vtbl, VSSetConstantBuffers1, 119);
CHECK(ID3D11DeviceContext1Vtbl, PSSetConstantBuffers1, 123);

CHECK(IDXGIFactoryVtbl, CreateSwapChain, 10);
CHECK(IDXGIFactory2Vtbl, CreateSwapChainForHwnd, 15);
CHECK(IDXGISwapChainVtbl, Present, 8);
CHECK(IDXGISwapChainVtbl, GetBuffer, 9);
CHECK(IDXGISwapChainVtbl, SetFullscreenState, 10);
CHECK(IDXGISwapChainVtbl, ResizeBuffers, 13);
CHECK(IDXGISwapChainVtbl, ResizeTarget, 14);
CHECK(IDXGIOutputVtbl, WaitForVBlank, 10);
