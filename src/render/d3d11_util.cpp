#include "render/d3d11_util.h"

#include "core/log.h"
#include "shaders/blit_ps.h"
#include "shaders/blit_vs.h"
#include "shaders/motion_vectors_cs.h"

#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace dsru::render::d3d11 {

namespace {

ComPtr<ID3D11ComputeShader> g_motionCs;
ComPtr<ID3D11VertexShader> g_blitVs;
ComPtr<ID3D11PixelShader> g_blitPs;
ComPtr<ID3D11SamplerState> g_point, g_linear;
ComPtr<ID3D11Buffer> g_constants;
ComPtr<ID3D11RasterizerState> g_rasterizer;
ComPtr<ID3D11BlendState> g_blend;
ComPtr<ID3D11DepthStencilState> g_depthState;
bool g_ready = false;

// Compute state the motion pass changes.
struct ComputeBackup {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11ComputeShader> shader;
    ID3D11ClassInstance* instances[256]{};
    UINT instanceCount = 256;
    ID3D11ShaderResourceView* srvs[2]{};
    ID3D11UnorderedAccessView* uavs[2]{};
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    explicit ComputeBackup(ID3D11DeviceContext* c) : context(c) {
        c->CSGetShader(&shader, instances, &instanceCount);
        c->CSGetShaderResources(0, 2, srvs);
        c->CSGetUnorderedAccessViews(0, 2, uavs);
        c->CSGetConstantBuffers(0, 1, &cb);
        c->CSGetSamplers(0, 1, &sampler);
    }
    ~ComputeBackup() {
        context->CSSetShader(shader.Get(), instances, instanceCount);
        context->CSSetShaderResources(0, 2, srvs);
        const UINT keep[2] = {~0u, ~0u};
        context->CSSetUnorderedAccessViews(0, 2, uavs, keep);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetSamplers(0, 1, &sampler);
        for (UINT i = 0; i < instanceCount; ++i)
            if (instances[i]) instances[i]->Release();
        for (auto* p : srvs)
            if (p) p->Release();
        for (auto* p : uavs)
            if (p) p->Release();
        if (cb) cb->Release();
        if (sampler) sampler->Release();
    }
};

// Graphics state a full-screen draw changes.
struct GraphicsBackup {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ID3D11ShaderResourceView* psSrv = nullptr;
    ID3D11SamplerState* psSampler = nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4]{};
    UINT sampleMask = 0;
    ComPtr<ID3D11DepthStencilState> depth;
    UINT stencilRef = 0;
    ComPtr<ID3D11RasterizerState> rasterizer;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    explicit GraphicsBackup(ID3D11DeviceContext* c) : context(c) {
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &psSrv);
        c->PSGetSamplers(0, 1, &psSampler);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&rasterizer);
        c->RSGetViewports(&viewportCount, viewports);
    }
    ~GraphicsBackup() {
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        context->GSSetShader(gs.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &psSrv);
        context->PSSetSamplers(0, 1, &psSampler);
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        context->OMSetDepthStencilState(depth.Get(), stencilRef);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(viewportCount, viewports);
        if (psSrv) psSrv->Release();
        if (psSampler) psSampler->Release();
        for (auto* p : rtvs)
            if (p) p->Release();
        if (dsv) dsv->Release();
    }
};

}  // namespace

bool Initialize(ID3D11Device* device) {
    if (g_ready) return true;
    HRESULT hr = device->CreateComputeShader(g_motion_vectors_cs, sizeof(g_motion_vectors_cs), nullptr, &g_motionCs);
    if (SUCCEEDED(hr)) hr = device->CreateVertexShader(g_blit_vs, sizeof(g_blit_vs), nullptr, &g_blitVs);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(g_blit_ps, sizeof(g_blit_ps), nullptr, &g_blitPs);
    D3D11_SAMPLER_DESC sampler{};
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sampler, &g_point);
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sampler, &g_linear);
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = (sizeof(MotionConstants) + 15) & ~15u;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&cb, nullptr, &g_constants);
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&raster, &g_rasterizer);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&blend, &g_blend);
    D3D11_DEPTH_STENCIL_DESC depth{};
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&depth, &g_depthState);
    if (FAILED(hr)) {
        log::Error("D3D11 helpers: creation failed (0x{:08x})", static_cast<unsigned>(hr));
        return false;
    }
    g_ready = true;
    return true;
}

void DispatchMotionVectors(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth,
                           ID3D11ShaderResourceView* velocity, ID3D11UnorderedAccessView* motionOut,
                           ID3D11UnorderedAccessView* depthOut, const MotionConstants& constants) {
    if (!g_ready) return;
    ComputeBackup backup(context);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(g_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(g_constants.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[2] = {depth, velocity};
    ID3D11UnorderedAccessView* uavs[2] = {motionOut, depthOut};
    context->CSSetShader(g_motionCs.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 2, srvs);
    context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cb = g_constants.Get();
    context->CSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState* sampler = g_point.Get();
    context->CSSetSamplers(0, 1, &sampler);
    const UINT w = static_cast<UINT>(constants.size[0]), h = static_cast<UINT>(constants.size[1]);
    context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    // Unbind the UAVs before the backup restores the game's views (a resource can't be both bound).
    ID3D11UnorderedAccessView* none[2]{};
    context->CSSetUnorderedAccessViews(0, 2, none, nullptr);
}

void Blit(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, UINT width,
          UINT height) {
    BlitRect(context, source, target, 0, 0, width, height);
}

void BlitRect(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, UINT x,
              UINT y, UINT width, UINT height) {
    if (!g_ready || !source || !target) return;
    GraphicsBackup backup(context);
    // The source may be bound as a render target by the game right now: unbind outputs first.
    context->OMSetRenderTargets(1, &target, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_blitVs.Get(), nullptr, 0);
    context->PSSetShader(g_blitPs.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* sampler = g_linear.Get();
    context->PSSetSamplers(0, 1, &sampler);
    context->OMSetBlendState(g_blend.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(g_depthState.Get(), 0);
    context->RSSetState(g_rasterizer.Get());
    const D3D11_VIEWPORT viewport{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                                  static_cast<float>(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
}

DXGI_FORMAT Typeless(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_TYPELESS;
        default: return f;
    }
}

bool UavStorable(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            return true;
        default: return false;
    }
}

bool TryCopy(ID3D11DeviceContext* context, ID3D11Resource* dst, ID3D11Resource* src) {
    ComPtr<ID3D11Texture2D> d, s;
    if (FAILED(dst->QueryInterface(IID_PPV_ARGS(&d))) || FAILED(src->QueryInterface(IID_PPV_ARGS(&s)))) return false;
    D3D11_TEXTURE2D_DESC dd, sd;
    d->GetDesc(&dd);
    s->GetDesc(&sd);
    if (dd.Width != sd.Width || dd.Height != sd.Height || sd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1)
        return false;
    if (Typeless(dd.Format) != Typeless(sd.Format) && dd.Format != sd.Format) return false;
    context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, nullptr);
    return true;
}

}  // namespace dsru::render::d3d11
