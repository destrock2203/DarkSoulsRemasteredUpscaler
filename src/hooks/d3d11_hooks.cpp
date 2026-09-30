#include "hooks/d3d11_hooks.h"

#include "core/config.h"
#include "core/log.h"
#include "game/shader_db.h"
#include "game/shader_ids.h"
#include "hooks/d3d11_vtables.h"
#include "hooks/hook_manager.h"
#include "proxy/proxy_host.h"
#include "render/upscale.h"

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace dsru::hooks::d3d11 {

namespace {

namespace shaders = game::shaders;

// Constant registers each role needs (float4 each): the TAA resolve reads up to cb0[78], the
// velocity vertex shaders' view-projections are c0-c15.
constexpr size_t kTaaConstantBytes = 80 * 16;
constexpr size_t kVelocityConstantBytes = 16 * 16;
// The final pass (brightness / gamma, FRPG_Menu output -> back buffer), compiled by the game itself.
constexpr uint64_t kPsGamma = 0x4915b254c739f466ull;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_immediate = nullptr;

// --- Shaders ---
std::shared_mutex g_shaderMutex;
std::unordered_map<void*, ShaderInfo> g_shaders;

ShaderInfo ShaderOf(void* shader) {
    if (!shader) return {};
    std::shared_lock lock(g_shaderMutex);
    const auto it = g_shaders.find(shader);
    return it != g_shaders.end() ? it->second : ShaderInfo{};
}

void RegisterShader(void* shader, shaders::Stage stage, const void* code, SIZE_T size) {
    ShaderInfo info;
    info.id = shaders::IdOf(code, size);
    const shaders::Info* entry = shaders::Find(info.id);
    const uint32_t flags = entry ? entry->flags : 0;
    if (stage == shaders::Stage::kPixel && info.id == game::shader_id::kPsResolveTaa)
        info.role = Role::TaaResolve;
    else if (stage == shaders::Stage::kPixel && (flags & shaders::kMenu))
        info.role = Role::Hud;
    else if (stage == shaders::Stage::kVertex && (flags & shaders::kVelocity))
        info.role = Role::Velocity;
    else if (stage == shaders::Stage::kPixel && info.id == kPsGamma)
        info.role = Role::Gamma;
    if (info.role == Role::TaaResolve) log::Info("TAA resolve shader created ({})", shader);
    std::unique_lock lock(g_shaderMutex);
    g_shaders[shader] = info;
}

// --- Contexts ---
constexpr size_t kMaxContexts = 64;
ContextState g_states[kMaxContexts];
std::atomic<size_t> g_stateCount{0};
std::mutex g_registerMutex;

ContextState* StateOf(ID3D11DeviceContext* context) {
    const size_t count = g_stateCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
        if (g_states[i].context == context) return &g_states[i];
    return nullptr;
}

ContextState* RegisterContext(ID3D11DeviceContext* context, bool deferred) {
    std::lock_guard lock(g_registerMutex);
    if (ContextState* s = StateOf(context)) return s;
    const size_t i = g_stateCount.load();
    if (i >= kMaxContexts) return nullptr;
    g_states[i].context = context;
    g_states[i].deferred = deferred;
    g_states[i].index = static_cast<uint8_t>(i);
    g_stateCount.store(i + 1, std::memory_order_release);
    return &g_states[i];
}

void ClearBindings(ContextState& s) {
    s.vs = s.ps = {};
    std::memset(s.psSrv, 0, sizeof(s.psSrv));
    std::memset(s.rtv, 0, sizeof(s.rtv));
    s.rtvCount = 0;
    s.dsv = nullptr;
    s.vsCb0 = s.psCb0 = nullptr;
}

// --- Constant capture ---
// Buffers seen bound to a role's draw: their updates are captured even when the shader isn't bound yet
// (the engine may write the constants before setting the shader).
constexpr size_t kWatch = 32;  // one constant buffer per deferred context (11 seen)
std::atomic<ID3D11Buffer*> g_watchPs[kWatch], g_watchVs[kWatch];

bool Watched(std::atomic<ID3D11Buffer*>* list, ID3D11Resource* resource) {
    for (size_t i = 0; i < kWatch; ++i)
        if (list[i].load(std::memory_order_relaxed) == resource) return true;
    return false;
}

void Watch(std::atomic<ID3D11Buffer*>* list, ID3D11Buffer* buffer) {
    if (!buffer || Watched(list, buffer)) return;
    for (size_t i = 0; i < kWatch; ++i) {
        ID3D11Buffer* expected = nullptr;
        if (list[i].compare_exchange_strong(expected, buffer)) return;
    }
    list[kWatch - 1] = buffer;  // full: replace the last
}

bool IsConstantBuffer(ID3D11Resource* resource, UINT& size) {
    D3D11_RESOURCE_DIMENSION dim;
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC desc;
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    size = desc.ByteWidth;
    return (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

// Which captured slot a write to `resource` goes to: 1 = PS (TAA), 2 = VS (velocity), 0 = none.
int CaptureTarget(const ContextState& s, ID3D11Resource* resource) {
    if (Watched(g_watchPs, resource)) return 1;
    if (Watched(g_watchVs, resource)) return 2;
    if (s.ps.role == Role::TaaResolve) return 1;
    if (s.vs.role == Role::Velocity) return 2;
    return 0;
}

void Capture(CapturedConstants& out, ID3D11Resource* resource, const void* data, size_t bytes, size_t wanted) {
    const size_t n = std::min(bytes, wanted);
    out.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    out.source = static_cast<ID3D11Buffer*>(resource);
}

// --- Command list splits ---
std::mutex g_splitMutex;
std::unordered_map<ID3D11CommandList*, std::vector<ContextState::Split>> g_splitLists;

using FinishCommandList_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
FinishCommandList_t g_finishOriginal = nullptr;  // the immediate variant's original (same for all)

// ---------------------------------------------------------------------------------------------
// Context hooks
// ---------------------------------------------------------------------------------------------

using SetCBs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using SetSRVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*,
                                               UINT);
using VSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*,
                                               ID3D11ClassInstance* const*, UINT);
using OMSetRTs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                            ID3D11DepthStencilView*);
using OMSetRTsUAVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                ID3D11DepthStencilView*, UINT, UINT,
                                                ID3D11UnorderedAccessView* const*, const UINT*);
using RSSetViewports_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorRects_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using Draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using Map_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using Unmap_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                     const void*, UINT, UINT);
using ClearState_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using ExecuteCommandList_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
// The game binds its constant buffers through ID3D11DeviceContext1 (offsets null: whole buffers).
using SetCBs1_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                                           const UINT*);

void OnSetRenderTargets(ContextState& s, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    std::memset(s.rtv, 0, sizeof(s.rtv));
    s.rtvCount = std::min<UINT>(count, 8);
    for (UINT i = 0; i < s.rtvCount; ++i) s.rtv[i] = rtvs ? rtvs[i] : nullptr;
    s.dsv = dsv;
}

// Before a draw: roles. Returns true when the draw was issued by the handler.
template <class DrawFn>
bool OnDraw(ID3D11DeviceContext* c, DrawFn&& draw) {
    ContextState* s = StateOf(c);
    if (!s) return false;
    if (s->vs.role == Role::Velocity) {
        if (s->vsConstants.source != s->vsCb0) Watch(g_watchVs, s->vsCb0);
        render::upscale::OnVelocityDraw(*s);
    }
    if (s->ps.role == Role::TaaResolve) {
        if (s->psConstants.source != s->psCb0) Watch(g_watchPs, s->psCb0);
        return render::upscale::OnTaaDraw(*s, std::function<void()>(draw));
    }
    if (s->ps.role == Role::Hud && !s->hudSeen) {
        s->hudSeen = true;
        render::upscale::OnHudDraw(*s);
    }
    if (s->ps.role == Role::Gamma) return render::upscale::OnGammaDraw(*s, std::function<void()>(draw));
    return false;
}

template <int V>
struct ContextHooks {
    static inline SetCBs_t oVSSetConstantBuffers, oPSSetConstantBuffers;
    static inline SetSRVs_t oPSSetShaderResources;
    static inline PSSetShader_t oPSSetShader;
    static inline VSSetShader_t oVSSetShader;
    static inline OMSetRTs_t oOMSetRenderTargets;
    static inline OMSetRTsUAVs_t oOMSetRenderTargetsAndUAVs;
    static inline Draw_t oDraw;
    static inline DrawIndexed_t oDrawIndexed;
    static inline DrawInstanced_t oDrawInstanced;
    static inline DrawIndexedInstanced_t oDrawIndexedInstanced;
    static inline Map_t oMap;
    static inline Unmap_t oUnmap;
    static inline UpdateSubresource_t oUpdateSubresource;
    static inline ClearState_t oClearState;
    static inline ExecuteCommandList_t oExecuteCommandList;
    static inline FinishCommandList_t oFinishCommandList;
    static inline SetCBs1_t oVSSetConstantBuffers1, oPSSetConstantBuffers1;
    static inline RSSetViewports_t oRSSetViewports;
    static inline RSSetScissorRects_t oRSSetScissorRects;

    // HUD redirect: the game's viewport / scissor, scaled while the redirect target is bound.
    static bool Redirected(const ContextState& s) { return s.redirectTo && s.rtv[0] == s.redirectTo; }
    static void ApplyViewport(ID3D11DeviceContext* c, ContextState& s) {
        if (s.hasViewport) {
            D3D11_VIEWPORT v = s.viewport;
            if (Redirected(s)) {
                v.TopLeftX *= s.redirectScaleX;
                v.Width *= s.redirectScaleX;
                v.TopLeftY *= s.redirectScaleY;
                v.Height *= s.redirectScaleY;
            }
            oRSSetViewports(c, 1, &v);
        }
        if (s.hasScissor) {
            D3D11_RECT r = s.scissor;
            if (Redirected(s)) {
                r.left = static_cast<LONG>(r.left * s.redirectScaleX);
                r.right = static_cast<LONG>(r.right * s.redirectScaleX + 0.5f);
                r.top = static_cast<LONG>(r.top * s.redirectScaleY);
                r.bottom = static_cast<LONG>(r.bottom * s.redirectScaleY + 0.5f);
            }
            oRSSetScissorRects(c, 1, &r);
        }
    }
    static void STDMETHODCALLTYPE RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps) {
        ContextState* s = StateOf(c);
        if (s) {
            s->hasViewport = n > 0 && vps;
            if (s->hasViewport) s->viewport = vps[0];
        }
        if (s && n == 1 && vps && Redirected(*s)) {
            const bool scissor = s->hasScissor;
            s->hasScissor = false;
            ApplyViewport(c, *s);
            s->hasScissor = scissor;
            return;
        }
        oRSSetViewports(c, n, vps);
    }
    static void STDMETHODCALLTYPE RSSetScissorRects(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* rects) {
        ContextState* s = StateOf(c);
        if (s) {
            s->hasScissor = n > 0 && rects;
            if (s->hasScissor) s->scissor = rects[0];
        }
        if (s && n == 1 && rects && Redirected(*s)) {
            const bool viewport = s->hasViewport;
            s->hasViewport = false;
            ApplyViewport(c, *s);
            s->hasViewport = viewport;
            return;
        }
        oRSSetScissorRects(c, n, rects);
    }

    static void STDMETHODCALLTYPE VSSetConstantBuffers1(ID3D11DeviceContext* c, UINT start, UINT n,
                                                       ID3D11Buffer* const* b, const UINT* first, const UINT* num) {
        if (start == 0 && n > 0)
            if (ContextState* s = StateOf(c)) s->vsCb0 = b ? b[0] : nullptr;
        oVSSetConstantBuffers1(c, start, n, b, first, num);
    }
    static void STDMETHODCALLTYPE PSSetConstantBuffers1(ID3D11DeviceContext* c, UINT start, UINT n,
                                                       ID3D11Buffer* const* b, const UINT* first, const UINT* num) {
        if (start == 0 && n > 0)
            if (ContextState* s = StateOf(c)) s->psCb0 = b ? b[0] : nullptr;
        oPSSetConstantBuffers1(c, start, n, b, first, num);
    }

    static void STDMETHODCALLTYPE VSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
        if (start == 0 && n > 0)
            if (ContextState* s = StateOf(c)) s->vsCb0 = b ? b[0] : nullptr;
        oVSSetConstantBuffers(c, start, n, b);
    }
    static void STDMETHODCALLTYPE PSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
        if (start == 0 && n > 0)
            if (ContextState* s = StateOf(c)) s->psCb0 = b ? b[0] : nullptr;
        oPSSetConstantBuffers(c, start, n, b);
    }
    static void STDMETHODCALLTYPE PSSetShaderResources(ID3D11DeviceContext* c, UINT start, UINT n,
                                                       ID3D11ShaderResourceView* const* v) {
        if (ContextState* s = StateOf(c))
            for (UINT i = 0; i < n; ++i)
                if (start + i < 8) s->psSrv[start + i] = v ? v[i] : nullptr;
        oPSSetShaderResources(c, start, n, v);
    }
    static void STDMETHODCALLTYPE PSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* sh, ID3D11ClassInstance* const* ci,
                                              UINT n) {
        if (ContextState* s = StateOf(c)) s->ps = ShaderOf(sh);
        oPSSetShader(c, sh, ci, n);
    }
    static void STDMETHODCALLTYPE VSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* sh,
                                              ID3D11ClassInstance* const* ci, UINT n) {
        if (ContextState* s = StateOf(c)) s->vs = ShaderOf(sh);
        oVSSetShader(c, sh, ci, n);
    }
    static void STDMETHODCALLTYPE OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                     ID3D11DepthStencilView* dsv) {
        ContextState* s = StateOf(c);
        if (!s) return oOMSetRenderTargets(c, n, rtvs, dsv);
        const bool wasRedirected = Redirected(*s);
        ID3D11RenderTargetView* substituted[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        if (s->redirectTo && n > 0 && n <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && rtvs && rtvs[0] == s->redirectFrom) {
            std::memcpy(substituted, rtvs, n * sizeof(*rtvs));
            substituted[0] = s->redirectTo;
            rtvs = substituted;
            dsv = nullptr;  // the game's depth buffer has the other size (the HUD draws without one)
        }
        OnSetRenderTargets(*s, n, rtvs, dsv);
        oOMSetRenderTargets(c, n, rtvs, dsv);
        if (wasRedirected != Redirected(*s)) ApplyViewport(c, *s);
    }
    static void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* c, UINT n,
                                                            ID3D11RenderTargetView* const* rtvs,
                                                            ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount,
                                                            ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
        if (ContextState* s = StateOf(c); s && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            OnSetRenderTargets(*s, n, rtvs, dsv);
        oOMSetRenderTargetsAndUAVs(c, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
    }
    static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c, UINT count, UINT start) {
        if (!OnDraw(c, [&] { oDraw(c, count, start); })) oDraw(c, count, start);
    }
    static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
        if (!OnDraw(c, [&] { oDrawIndexed(c, count, start, base); })) oDrawIndexed(c, count, start, base);
    }
    static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start,
                                                UINT startInstance) {
        if (!OnDraw(c, [&] { oDrawInstanced(c, count, instances, start, startInstance); }))
            oDrawInstanced(c, count, instances, start, startInstance);
    }
    static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start,
                                                       INT base, UINT startInstance) {
        if (!OnDraw(c, [&] { oDrawIndexedInstanced(c, count, instances, start, base, startInstance); }))
            oDrawIndexedInstanced(c, count, instances, start, base, startInstance);
    }
    static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                         D3D11_MAPPED_SUBRESOURCE* mapped) {
        if (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)
            if (ContextState* s = StateOf(c); s && s->splitActive) {
                const bool seen = std::find(s->discarded.begin(), s->discarded.end(), r) != s->discarded.end();
                if (!seen) {
                    s->discarded.push_back(r);
                    type = D3D11_MAP_WRITE_DISCARD;  // the first map of this resource in the new list
                }
            }
        const HRESULT hr = oMap(c, r, sub, type, flags, mapped);
        if (SUCCEEDED(hr) && mapped && type != D3D11_MAP_READ)
            if (ContextState* s = StateOf(c))
                if (const int target = CaptureTarget(*s, r)) {
                    UINT size = 0;
                    if (IsConstantBuffer(r, size)) {
                        s->mapped = r;
                        s->mappedData = mapped->pData;
                        s->mappedForPs = target == 1;
                    }
                }
        return hr;
    }
    static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
        if (ContextState* s = StateOf(c); s && s->mapped == r && s->mappedData) {
            UINT size = 0;
            IsConstantBuffer(r, size);
            if (s->mappedForPs)
                Capture(s->psConstants, r, s->mappedData, size, kTaaConstantBytes);
            else
                Capture(s->vsConstants, r, s->mappedData, size, kVelocityConstantBytes);
            s->mapped = nullptr;
            s->mappedData = nullptr;
        }
        oUnmap(c, r, sub);
    }
    static void STDMETHODCALLTYPE UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                                                    const D3D11_BOX* box, const void* data, UINT rowPitch,
                                                    UINT depthPitch) {
        if (data && !box)
            if (ContextState* s = StateOf(c))
                if (const int target = CaptureTarget(*s, r)) {
                    UINT size = 0;
                    if (IsConstantBuffer(r, size)) {
                        if (target == 1)
                            Capture(s->psConstants, r, data, size, kTaaConstantBytes);
                        else
                            Capture(s->vsConstants, r, data, size, kVelocityConstantBytes);
                    }
                }
        oUpdateSubresource(c, r, sub, box, data, rowPitch, depthPitch);
    }
    static void STDMETHODCALLTYPE ClearState(ID3D11DeviceContext* c) {
        if (ContextState* s = StateOf(c)) ClearBindings(*s);
        oClearState(c);
    }
    static void STDMETHODCALLTYPE ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore) {
        std::vector<ContextState::Split> splits;
        if (c == g_immediate) {
            std::lock_guard lock(g_splitMutex);
            if (const auto it = g_splitLists.find(list); it != g_splitLists.end()) {
                splits = std::move(it->second);
                g_splitLists.erase(it);
            }
        }
        for (ContextState::Split& split : splits) {
            if (!split.prefix) continue;
            oExecuteCommandList(c, split.prefix, TRUE);
            split.prefix->Release();
            if (split.atExecute) split.atExecute();
        }
        oExecuteCommandList(c, list, restore);
        if (!restore)
            if (ContextState* s = StateOf(c)) ClearBindings(*s);
        for (ContextState::Split& split : splits)
            if (!split.prefix && split.atExecute) split.atExecute();
    }
    static HRESULT STDMETHODCALLTYPE FinishCommandList(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** list) {
        const HRESULT hr = oFinishCommandList(c, restore, list);
        if (ContextState* s = StateOf(c)) {
            if (SUCCEEDED(hr) && list && *list && !s->splits.empty()) {
                std::lock_guard lock(g_splitMutex);
                g_splitLists[*list] = std::move(s->splits);
                if (g_splitLists.size() > 64) {  // lists never executed: drop them
                    for (auto& [l, pending] : g_splitLists)
                        for (auto& split : pending)
                            if (split.prefix) split.prefix->Release();
                    g_splitLists.clear();
                }
            }
            s->splits.clear();
            s->splitActive = false;
            s->discarded.clear();
            s->redirectFrom = s->redirectTo = nullptr;
            s->hudSeen = false;
            if (!restore) ClearBindings(*s);
        }
        return hr;
    }

    static void Install(ID3D11DeviceContext* context, const char* label) {
        auto slot = [&](size_t index) { return VTableEntry(context, index); };
        const std::string p = std::string("ID3D11DeviceContext(") + label + ")::";
        Batch batch;
        hooks::Install(slot(vt::kContext_VSSetConstantBuffers), &VSSetConstantBuffers, &oVSSetConstantBuffers, (p + "VSSetConstantBuffers").c_str());
        hooks::Install(slot(vt::kContext_PSSetConstantBuffers), &PSSetConstantBuffers, &oPSSetConstantBuffers, (p + "PSSetConstantBuffers").c_str());
        hooks::Install(slot(vt::kContext_PSSetShaderResources), &PSSetShaderResources, &oPSSetShaderResources, (p + "PSSetShaderResources").c_str());
        hooks::Install(slot(vt::kContext_PSSetShader), &PSSetShader, &oPSSetShader, (p + "PSSetShader").c_str());
        hooks::Install(slot(vt::kContext_VSSetShader), &VSSetShader, &oVSSetShader, (p + "VSSetShader").c_str());
        hooks::Install(slot(vt::kContext_OMSetRenderTargets), &OMSetRenderTargets, &oOMSetRenderTargets, (p + "OMSetRenderTargets").c_str());
        hooks::Install(slot(vt::kContext_OMSetRenderTargetsAndUnorderedAccessViews), &OMSetRenderTargetsAndUAVs, &oOMSetRenderTargetsAndUAVs, (p + "OMSetRenderTargetsAndUnorderedAccessViews").c_str());
        hooks::Install(slot(vt::kContext_Draw), &Draw, &oDraw, (p + "Draw").c_str());
        hooks::Install(slot(vt::kContext_DrawIndexed), &DrawIndexed, &oDrawIndexed, (p + "DrawIndexed").c_str());
        hooks::Install(slot(vt::kContext_DrawInstanced), &DrawInstanced, &oDrawInstanced, (p + "DrawInstanced").c_str());
        hooks::Install(slot(vt::kContext_DrawIndexedInstanced), &DrawIndexedInstanced, &oDrawIndexedInstanced, (p + "DrawIndexedInstanced").c_str());
        hooks::Install(slot(vt::kContext_Map), &Map, &oMap, (p + "Map").c_str());
        hooks::Install(slot(vt::kContext_Unmap), &Unmap, &oUnmap, (p + "Unmap").c_str());
        hooks::Install(slot(vt::kContext_UpdateSubresource), &UpdateSubresource, &oUpdateSubresource, (p + "UpdateSubresource").c_str());
        hooks::Install(slot(vt::kContext_ClearState), &ClearState, &oClearState, (p + "ClearState").c_str());
        hooks::Install(slot(vt::kContext_ExecuteCommandList), &ExecuteCommandList, &oExecuteCommandList, (p + "ExecuteCommandList").c_str());
        hooks::Install(slot(vt::kContext_FinishCommandList), &FinishCommandList, &oFinishCommandList, (p + "FinishCommandList").c_str());
        hooks::Install(slot(vt::kContext_RSSetViewports), &RSSetViewports, &oRSSetViewports, (p + "RSSetViewports").c_str());
        hooks::Install(slot(vt::kContext_RSSetScissorRects), &RSSetScissorRects, &oRSSetScissorRects, (p + "RSSetScissorRects").c_str());
        hooks::Install(slot(vt::kContext1_VSSetConstantBuffers1), &VSSetConstantBuffers1, &oVSSetConstantBuffers1, (p + "VSSetConstantBuffers1").c_str());
        hooks::Install(slot(vt::kContext1_PSSetConstantBuffers1), &PSSetConstantBuffers1, &oPSSetConstantBuffers1, (p + "PSSetConstantBuffers1").c_str());
        if (!g_finishOriginal) g_finishOriginal = oFinishCommandList;
    }
};

// Which variant's FinishCommandList original to call for a context (SplitHere).
FinishCommandList_t g_deferredFinishOriginal = nullptr;

// ---------------------------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------------------------

using CreateVS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11VertexShader**);
using CreatePS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11PixelShader**);
using CreateDeferredContext_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
CreateVS_t oCreateVS;
CreatePS_t oCreatePS;
CreateDeferredContext_t oCreateDeferredContext;
CreateTexture2D_t oCreateTexture2D;

HRESULT STDMETHODCALLTYPE hkCreateVS(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* link,
                                     ID3D11VertexShader** out) {
    const HRESULT hr = oCreateVS(d, code, size, link, out);
    if (SUCCEEDED(hr) && out && *out) RegisterShader(*out, shaders::Stage::kVertex, code, size);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreatePS(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* link,
                                     ID3D11PixelShader** out) {
    const HRESULT hr = oCreatePS(d, code, size, link, out);
    if (SUCCEEDED(hr) && out && *out) RegisterShader(*out, shaders::Stage::kPixel, code, size);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateTexture2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc,
                                            const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out) {
    const HRESULT hr = oCreateTexture2D(d, desc, data, out);
    if (SUCCEEDED(hr) && desc && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
        log::Debug("Texture {}x{} format {} bind 0x{:x} samples {}", desc->Width, desc->Height,
                   static_cast<int>(desc->Format), desc->BindFlags, desc->SampleDesc.Count);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out) {
    const HRESULT hr = oCreateDeferredContext(d, flags, out);
    if (SUCCEEDED(hr) && out && *out) {
        RegisterContext(*out, true);
        static std::once_flag once;
        std::call_once(once, [&] {
            bool differs = false;
            for (size_t slot : {vt::kContext_Draw, vt::kContext_DrawIndexed, vt::kContext_Map, vt::kContext_Unmap,
                                vt::kContext_FinishCommandList, vt::kContext_PSSetShader})
                differs |= VTableEntry(*out, slot) != VTableEntry(g_immediate, slot);
            if (differs) {
                log::Info("Deferred contexts have their own method implementations: hooking them too");
                ContextHooks<1>::Install(*out, "deferred");
                g_deferredFinishOriginal = ContextHooks<1>::oFinishCommandList;
            }
        });
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// D3D11CreateDevice
// ---------------------------------------------------------------------------------------------

using D3D11CreateDevice_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                             UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
D3D11CreateDevice_t oD3D11CreateDevice = nullptr;

HRESULT WINAPI hkD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                   const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk, ID3D11Device** device,
                                   D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    proxy::EnsureInitialized();
    const HRESULT hr = oD3D11CreateDevice(adapter, type, software, flags, levels, levelCount, sdk, device, level, context);
    if (FAILED(hr) || !device || !*device || !context || !*context || g_device || !config::Get().enabled) return hr;

    g_device = *device;
    g_immediate = *context;
    log::Info("Game D3D11 device {} (feature level {:x}, flags 0x{:x})", static_cast<void*>(g_device),
              static_cast<int>(g_device->GetFeatureLevel()), flags);
    RegisterContext(g_immediate, false);
    {
        Batch batch;
        Install(VTableEntry(g_device, vt::kDevice_CreateVertexShader), &hkCreateVS, &oCreateVS, "ID3D11Device::CreateVertexShader");
        Install(VTableEntry(g_device, vt::kDevice_CreatePixelShader), &hkCreatePS, &oCreatePS, "ID3D11Device::CreatePixelShader");
        Install(VTableEntry(g_device, vt::kDevice_CreateDeferredContext), &hkCreateDeferredContext, &oCreateDeferredContext,
                "ID3D11Device::CreateDeferredContext");
        if (config::Get().logResources)
            Install(VTableEntry(g_device, vt::kDevice_CreateTexture2D), &hkCreateTexture2D, &oCreateTexture2D,
                    "ID3D11Device::CreateTexture2D");
    }
    ContextHooks<0>::Install(g_immediate, "immediate");
    render::upscale::OnDeviceCreated(g_device, g_immediate);
    return hr;
}

}  // namespace

void InstallEarlyHooks() {
    PatchImport(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDevice", &hkD3D11CreateDevice, &oD3D11CreateDevice);
}

ID3D11Device* Device() { return g_device; }
ID3D11DeviceContext* Immediate() { return g_immediate; }

void SplitHere(ContextState& state, std::function<void()> atExecute) {
    if (!state.deferred) {
        atExecute();
        return;
    }
    FinishCommandList_t finish = g_deferredFinishOriginal ? g_deferredFinishOriginal : g_finishOriginal;
    ID3D11CommandList* prefix = nullptr;
    if (!finish || FAILED(finish(state.context, TRUE, &prefix)) || !prefix) {
        log::Error("Splitting a deferred command list failed - the upscaler is skipped for this frame");
        return;
    }
    state.splits.push_back({prefix, std::move(atExecute)});
    state.splitActive = true;
    state.discarded.clear();
}

void BeginRedirect(ContextState& state, ID3D11RenderTargetView* from, ID3D11RenderTargetView* to, float scaleX,
                   float scaleY) {
    state.redirectFrom = from;
    state.redirectTo = to;
    state.redirectScaleX = scaleX;
    state.redirectScaleY = scaleY;
    // Re-bind through the hooks: the substitution and the scaled viewport apply now.
    ID3D11RenderTargetView* rtvs[8];
    std::memcpy(rtvs, state.rtv, sizeof(rtvs));
    state.context->OMSetRenderTargets(state.rtvCount, rtvs, state.dsv);
}

void EndRedirect(ContextState& state) {
    if (!state.redirectTo) return;
    const bool bound = state.rtv[0] == state.redirectTo;
    ID3D11RenderTargetView* from = state.redirectFrom;
    state.redirectFrom = state.redirectTo = nullptr;
    if (bound) {
        ID3D11RenderTargetView* rtvs[8];
        std::memcpy(rtvs, state.rtv, sizeof(rtvs));
        rtvs[0] = from;
        state.context->OMSetRenderTargets(state.rtvCount, rtvs, state.dsv);
        if (state.hasViewport) state.context->RSSetViewports(1, &state.viewport);
        if (state.hasScissor) state.context->RSSetScissorRects(1, &state.scissor);
    }
}

ContextState* ImmediateState() { return g_immediate ? StateOf(g_immediate) : nullptr; }

void AtExecute(ContextState& state, std::function<void()> fn) {
    if (!state.deferred) {
        fn();
        return;
    }
    state.splits.push_back({nullptr, std::move(fn)});
}

}  // namespace dsru::hooks::d3d11
