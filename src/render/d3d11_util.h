#pragma once

#include <d3d11.h>

#include <cstdint>

// D3D11 work the mod records into the game's contexts (immediate or deferred): the motion vector
// pass and full-screen copies. Each call saves the pipeline state it changes and restores it, so the
// game's following commands see the state they set.
namespace dsru::render::d3d11 {

bool Initialize(ID3D11Device* device);

struct MotionConstants {
    float size[4];               // render width, height, 1 / width, 1 / height
    float clipToWorld[4][4];     // rows
    float worldToPrevClip[4][4]; // rows
    float cameraOffset[4];
    uint32_t flags[4];           // [0]: velocity bound
};

// shaders/motion_vectors.hlsl over width x height.
void DispatchMotionVectors(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth,
                           ID3D11ShaderResourceView* velocity, ID3D11UnorderedAccessView* motionOut,
                           ID3D11UnorderedAccessView* depthOut, const MotionConstants& constants);

// Draws `source` over the whole of `target` (width x height), converting format / scaling.
void Blit(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, UINT width,
          UINT height);
// The same into the rectangle (x, y, width, height) of `target` (the rest is left as it is).
void BlitRect(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, UINT x,
              UINT y, UINT width, UINT height);

// CopySubresourceRegion when `dst` / `src` are copy-compatible (same format family and size),
// otherwise false (the caller blits).
bool TryCopy(ID3D11DeviceContext* context, ID3D11Resource* dst, ID3D11Resource* src);

// Typeless variant of a format (R11G11B10 / R10G10B10A2 / R16G16B16A16 ... families) for shared copies.
DXGI_FORMAT Typeless(DXGI_FORMAT format);
// A format a compute shader can write through a typed UAV on every D3D11/12 GPU.
bool UavStorable(DXGI_FORMAT format);

}  // namespace dsru::render::d3d11
