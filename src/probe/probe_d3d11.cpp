// DSRProbe: D3D11 / DXGI hooks. Registers every resource, view and shader the game creates, tracks
// the pipeline state of the immediate and deferred contexts, logs the frame's post-processing
// passes, and records frame traces (probe_trace.cpp writes them).

#include "core/log.h"
#include "core/paths.h"
#include "game/shader_db.h"
#include "game/shader_ids.h"
#include "hooks/d3d11_vtables.h"
#include "hooks/hook_manager.h"
#include "probe/probe.h"
#include "probe/probe_internal.h"
#include "proxy/proxy_host.h"

#include <Windows.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <dxgi1_2.h>
#include <intrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dsru::probe {

using namespace detail;
namespace shaders = game::shaders;
namespace vt = hooks::vt;

namespace {

// ---------------------------------------------------------------------------------------------
// Registries
// ---------------------------------------------------------------------------------------------

struct ViewInfo {
    char kind = '?';  // 'S' SRV, 'R' RTV, 'D' DSV, 'U' UAV
    ID3D11Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

std::shared_mutex g_registryMutex;
std::unordered_map<ID3D11Resource*, ResourceInfo> g_resources;
std::unordered_map<void*, ViewInfo> g_views;
std::unordered_map<void*, ShaderRecord*> g_shaders;
std::deque<ShaderRecord> g_shaderStore;
std::atomic<uint32_t> g_nextResourceId{1};
std::atomic<uint64_t> g_presents{0};
ID3D11Resource* g_backBuffer = nullptr;

ResourceInfo Describe(ID3D11Resource* resource) {
    ResourceInfo info;
    resource->GetType(&info.dimension);
    if (info.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&tex)))) {
            D3D11_TEXTURE2D_DESC d;
            tex->GetDesc(&d);
            info.width = d.Width;
            info.height = d.Height;
            info.arraySize = d.ArraySize;
            info.mips = d.MipLevels;
            info.samples = d.SampleDesc.Count;
            info.format = d.Format;
            info.bind = d.BindFlags;
            info.misc = d.MiscFlags;
            info.usage = d.Usage;
            info.cpuAccess = d.CPUAccessFlags;
        }
    } else if (info.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        ComPtr<ID3D11Buffer> buffer;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) {
            D3D11_BUFFER_DESC d;
            buffer->GetDesc(&d);
            info.byteWidth = d.ByteWidth;
            info.bind = d.BindFlags;
            info.misc = d.MiscFlags;
            info.usage = d.Usage;
            info.cpuAccess = d.CPUAccessFlags;
        }
    }
    return info;
}

// Registers (or re-registers, a reused address) a resource. Exclusive lock taken inside.
ResourceInfo Register(ID3D11Resource* resource, const void* caller) {
    ResourceInfo info = Describe(resource);
    info.id = g_nextResourceId++;
    info.createdAtPresent = g_presents.load();
    info.caller = caller ? DescribeAddress(caller) : std::string("?");
    std::unique_lock lock(g_registryMutex);
    g_resources[resource] = info;
    return info;
}

// The registered info, registering resources created before the hooks (or by others) lazily.
ResourceInfo ResourceOf(ID3D11Resource* resource) {
    if (!resource) return {};
    {
        std::shared_lock lock(g_registryMutex);
        if (const auto it = g_resources.find(resource); it != g_resources.end()) return it->second;
    }
    return Register(resource, nullptr);
}

uint32_t IdOf(ID3D11Resource* resource) { return resource ? ResourceOf(resource).id : 0; }

ViewInfo ViewOf(ID3D11View* view) {
    if (!view) return {};
    {
        std::shared_lock lock(g_registryMutex);
        if (const auto it = g_views.find(view); it != g_views.end()) return it->second;
    }
    // A view created before the hooks: its resource (the view holds a reference; GetResource adds one).
    ViewInfo info;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    info.resource = resource.Get();
    std::unique_lock lock(g_registryMutex);
    g_views[view] = info;
    return info;
}

const ShaderRecord* ShaderOf(void* shader) {
    if (!shader) return nullptr;
    std::shared_lock lock(g_registryMutex);
    const auto it = g_shaders.find(shader);
    return it != g_shaders.end() ? it->second : nullptr;
}

// --- Shader reflection (d3dcompiler_47.dll, loaded on first use) ---

using D3DReflect_t = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);

D3DReflect_t Reflector() {
    static D3DReflect_t fn = [] {
        HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        auto f = module ? reinterpret_cast<D3DReflect_t>(GetProcAddress(module, "D3DReflect")) : nullptr;
        if (!f) log::Warn("d3dcompiler_47.dll unavailable: shader constants won't be named");
        return f;
    }();
    return fn;
}

void Reflect(const void* bytecode, SIZE_T size, ShaderRecord& record) {
    const D3DReflect_t reflect = Reflector();
    if (!reflect) return;
    ComPtr<ID3D11ShaderReflection> r;
    if (FAILED(reflect(bytecode, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(r.GetAddressOf()))))
        return;
    D3D11_SHADER_DESC desc{};
    if (FAILED(r->GetDesc(&desc))) return;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(r->GetResourceBindingDesc(i, &b))) continue;
        record.bindings.push_back({b.Name ? b.Name : "?", b.Type, b.BindPoint, b.BindCount});
        if (b.Type != D3D_SIT_CBUFFER) continue;
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName(b.Name);
        D3D11_SHADER_BUFFER_DESC cbDesc{};
        if (!cb || FAILED(cb->GetDesc(&cbDesc))) continue;
        for (UINT v = 0; v < cbDesc.Variables; ++v) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC vd{};
            if (!var || FAILED(var->GetDesc(&vd)) || !(vd.uFlags & D3D_SVF_USED)) continue;
            D3D11_SHADER_TYPE_DESC td{};
            if (ID3D11ShaderReflectionType* type = var->GetType()) type->GetDesc(&td);
            record.variables.push_back({vd.Name ? vd.Name : "?", b.BindPoint, vd.StartOffset, vd.Size,
                                        static_cast<uint8_t>(td.Class), static_cast<uint8_t>(td.Type),
                                        static_cast<uint16_t>(td.Rows), static_cast<uint16_t>(td.Columns),
                                        static_cast<uint16_t>(td.Elements)});
        }
    }
}

void RegisterShader(void* shader, shaders::Stage stage, const void* bytecode, SIZE_T size) {
    const uint64_t id = shaders::IdOf(bytecode, size);
    const shaders::Info* info = shaders::Find(id);
    std::unique_lock lock(g_registryMutex);
    ShaderRecord& record = g_shaderStore.emplace_back();
    record.index = static_cast<uint32_t>(g_shaderStore.size());
    record.id = id;
    record.stage = stage;
    record.info = info;
    record.name = info ? std::string(shaders::FirstName(info)) : std::format("unknown_{:016x}", id);
    lock.unlock();
    Reflect(bytecode, size, record);
    lock.lock();
    g_shaders[shader] = &record;
    lock.unlock();

    if (!info) {
        static std::atomic<int> logged{0};
        if (logged++ < 200)
            log::Info("Shader S{} {} not in the game's table (id {:016x}, {} bytes)", record.index,
                      stage == shaders::Stage::kVertex ? "VS" : stage == shaders::Stage::kPixel ? "PS" : "CS", id,
                      size);
        if (Get().dumpUnknownShaders) {
            std::error_code ec;
            const auto dir = OutputDirectory() / "DSRProbe_shaders";
            std::filesystem::create_directories(dir, ec);
            std::ofstream(dir / std::format("{:016x}.{}.cso", id,
                                            stage == shaders::Stage::kVertex ? "vs" : stage == shaders::Stage::kPixel ? "ps" : "cs"),
                          std::ios::binary)
                .write(static_cast<const char*>(bytecode), static_cast<std::streamsize>(size));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Context state
// ---------------------------------------------------------------------------------------------

// Pass-sequence tokens: a shader index (named filter / deferred passes) or one of these.
constexpr uint32_t kTokenMenu = 0x80000001u;
constexpr uint32_t kTokenVelocity = 0x80000002u;
constexpr uint32_t kTokenModel = 0x80000003u;
constexpr uint32_t kTokenSfx = 0x80000004u;
constexpr uint32_t kTokenOther = 0x80000005u;

struct ContextState {
    ID3D11DeviceContext* context = nullptr;
    uint8_t index = 0;  // 0 = immediate
    const ShaderRecord* vs = nullptr;
    const ShaderRecord* ps = nullptr;
    const ShaderRecord* cs = nullptr;
    ID3D11RenderTargetView* rtv[8]{};
    UINT rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* psSrv[32]{};
    ID3D11ShaderResourceView* csSrv[16]{};
    ID3D11UnorderedAccessView* csUav[8]{};
    ID3D11Buffer* vsCb[4]{};
    ID3D11Buffer* psCb[4]{};
    ID3D11Buffer* csCb[4]{};
    D3D11_VIEWPORT viewport{};
    uint32_t draws = 0;                 // since the last command list / frame
    std::vector<uint32_t> tokens;       // since the last command list / frame
    // Tracing only.
    std::vector<Event> events;
    std::unordered_map<ID3D11Resource*, void*> mapped;
    std::unordered_map<ID3D11Buffer*, Snapshot> latest;
    int menuDumps = 0;

    void ClearBindings() {
        vs = ps = cs = nullptr;
        std::memset(rtv, 0, sizeof(rtv));
        rtvCount = 0;
        dsv = nullptr;
        std::memset(psSrv, 0, sizeof(psSrv));
        std::memset(csSrv, 0, sizeof(csSrv));
        std::memset(csUav, 0, sizeof(csUav));
        std::memset(vsCb, 0, sizeof(vsCb));
        std::memset(psCb, 0, sizeof(psCb));
        std::memset(csCb, 0, sizeof(csCb));
        viewport = {};
    }
};

constexpr size_t kMaxContexts = 64;
ContextState g_contextStates[kMaxContexts];
std::atomic<size_t> g_contextCount{0};
std::mutex g_contextRegisterMutex;

ContextState* StateOf(ID3D11DeviceContext* context) {
    const size_t count = g_contextCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
        if (g_contextStates[i].context == context) return &g_contextStates[i];
    return nullptr;
}

ContextState* RegisterContext(ID3D11DeviceContext* context) {
    std::lock_guard lock(g_contextRegisterMutex);
    if (ContextState* existing = StateOf(context)) return existing;
    const size_t i = g_contextCount.load();
    if (i >= kMaxContexts) return nullptr;
    g_contextStates[i].context = context;
    g_contextStates[i].index = static_cast<uint8_t>(i);
    g_contextCount.store(i + 1, std::memory_order_release);
    return &g_contextStates[i];
}

// ---------------------------------------------------------------------------------------------
// Frames, command lists, tracing
// ---------------------------------------------------------------------------------------------

struct CommandListRecord {
    uint32_t id = 0;
    uint8_t context = 0;
    uint32_t draws = 0;
    std::vector<uint32_t> tokens;
    std::vector<Event> events;
};

std::mutex g_listMutex;
std::unordered_map<ID3D11CommandList*, CommandListRecord> g_lists;
std::atomic<uint32_t> g_nextListId{1};

enum class TraceState { Idle, Warmup, Recording };
std::atomic<TraceState> g_traceState{TraceState::Idle};
std::atomic<bool> g_tracing{false};  // events are recorded
int g_traceFramesLeft = 0;
std::mutex g_frameMutex;             // g_frame, g_frameTokens (immediate context + present thread)
FrameRecord g_frame;
std::vector<FrameRecord> g_tracedFrames;
std::vector<uint32_t> g_frameTokens;
uint32_t g_frameDraws = 0, g_frameLists = 0;
std::atomic<int> g_dumpsLeft{0};
// Constant buffer updates during a traced frame, by kind (logged at the frame's end).
std::atomic<uint32_t> g_cbMapDiscard{0}, g_cbMapNoOverwrite{0}, g_cbMapOther{0}, g_cbUpdates{0}, g_cbUpdates1{0};
std::atomic<uint32_t> g_cbBinds1{0};  // *SetConstantBuffers1 calls (D3D11.1 offsets), whole session

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_immediate = nullptr;
IDXGISwapChain* g_swapChain = nullptr;

void AppendImmediate(Event&& e) {
    std::lock_guard lock(g_frameMutex);
    g_frame.events.push_back(std::move(e));
}

void AddEvent(ContextState& s, Event&& e) {
    if (s.index == 0)
        AppendImmediate(std::move(e));
    else
        s.events.push_back(std::move(e));
}

Snapshot LatestOf(ContextState& s, ID3D11Buffer* buffer) {
    if (!buffer) return {};
    const auto it = s.latest.find(buffer);
    return it != s.latest.end() ? it->second : Snapshot{};
}

void FillTargets(ContextState& s, Event& e) {
    e.rtCount = static_cast<uint8_t>(std::min<UINT>(s.rtvCount, 8));
    for (UINT i = 0; i < e.rtCount; ++i) {
        if (!s.rtv[i]) continue;
        const ViewInfo v = ViewOf(s.rtv[i]);
        e.rt[i] = IdOf(v.resource);
        e.rtFormat[i] = v.format;
    }
    if (s.dsv) {
        const ViewInfo v = ViewOf(s.dsv);
        e.ds = IdOf(v.resource);
        e.dsFormat = v.format;
    }
    e.viewport = s.viewport;
}

void FillSrvs(ID3D11ShaderResourceView* const* srvs, size_t count, std::vector<BoundResource>& out) {
    for (size_t i = 0; i < count; ++i) {
        if (!srvs[i]) continue;
        const ViewInfo v = ViewOf(srvs[i]);
        out.push_back({static_cast<uint8_t>(i), IdOf(v.resource), v.format});
    }
}

// Shader flags of the draw's pixel shader, for the pass sequence.
void NoteToken(ContextState& s) {
    const ShaderRecord* ps = s.ps;
    const uint32_t flags = ps ? ps->flags() : 0;
    uint32_t token;
    if (flags & (shaders::kFilter | shaders::kDeferred))
        token = ps->index;
    else if (flags & shaders::kMenu)
        token = kTokenMenu;
    else if ((flags & shaders::kVelocity) || (s.vs && (s.vs->flags() & shaders::kVelocity)))
        token = kTokenVelocity;
    else if (flags & shaders::kModel)
        token = kTokenModel;
    else if (flags & shaders::kSfx)
        token = kTokenSfx;
    else
        token = kTokenOther;
    // Consecutive model / sfx / other / menu draws are summarized: only filter passes repeat.
    if (token >= 0x80000000u && !s.tokens.empty() && s.tokens.back() == token) return;
    if (s.tokens.size() < 4096) s.tokens.push_back(token);
}

ID3D11Resource* RenderTarget0(ContextState& s) {
    if (!s.rtvCount || !s.rtv[0]) return nullptr;
    return ViewOf(s.rtv[0]).resource;
}

void BeforeDraw(ID3D11DeviceContext* context, bool indexed, uint32_t count, uint32_t instances) {
    ContextState* s = StateOf(context);
    if (!s) return;
    ++s->draws;
    NoteToken(*s);
    if (!g_tracing.load(std::memory_order_relaxed)) return;

    const uint32_t flags = s->ps ? s->ps->flags() : 0;
    // HUD-less image: the render target right before the frame's first HUD draw.
    if (Get().dumpTextures && (flags & shaders::kMenu) && s->menuDumps == 0 && g_dumpsLeft > 0) {
        ++s->menuDumps;
        if (ID3D11Resource* rt = RenderTarget0(*s)) {
            --g_dumpsLeft;
            dump::Capture(context, rt, std::format("R{}_before_first_HUD_draw", IdOf(rt)));
        }
    }
    // The TAA resolve's inputs: color, history, depth, velocity, ...
    if (Get().dumpTextures && s->ps && s->ps->id == game::shader_id::kPsResolveTaa) {
        for (UINT slot = 0; slot < 8; ++slot) {
            if (!s->psSrv[slot] || g_dumpsLeft <= 0) continue;
            const ViewInfo v = ViewOf(s->psSrv[slot]);
            if (!v.resource) continue;
            --g_dumpsLeft;
            dump::Capture(context, v.resource, std::format("R{}_ResolveTAA_input_t{}", IdOf(v.resource), slot));
        }
    }

    Event e;
    e.type = EventType::Draw;
    e.context = s->index;
    e.indexed = indexed;
    e.count = count;
    e.instances = instances;
    e.vs = s->vs;
    e.ps = s->ps;
    FillTargets(*s, e);
    FillSrvs(s->psSrv, std::size(s->psSrv), e.srvs);
    e.vsCb[0] = LatestOf(*s, s->vsCb[0]);
    e.vsCb[1] = LatestOf(*s, s->vsCb[1]);
    e.psCb[0] = LatestOf(*s, s->psCb[0]);
    e.psCb[1] = LatestOf(*s, s->psCb[1]);
    for (int i = 0; i < 2; ++i) {
        e.vsCbId[i] = IdOf(s->vsCb[i]);
        e.psCbId[i] = IdOf(s->psCb[i]);
    }
    AddEvent(*s, std::move(e));
}

void AfterDraw(ID3D11DeviceContext* context) {
    if (!g_tracing.load(std::memory_order_relaxed) || !Get().dumpTextures || g_dumpsLeft <= 0) return;
    ContextState* s = StateOf(context);
    if (!s || !s->ps) return;
    const uint32_t flags = s->ps->flags();
    if (!(flags & (shaders::kFilter | shaders::kDeferred))) return;
    ID3D11Resource* rt = RenderTarget0(*s);
    if (!rt) return;
    --g_dumpsLeft;
    dump::Capture(context, rt, std::format("R{}_after_{}", IdOf(rt), s->ps->name));
}

// ---------------------------------------------------------------------------------------------
// Context hooks (V: a second set of detours for deferred contexts when their methods differ)
// ---------------------------------------------------------------------------------------------

using SetCBs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using SetSRVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using SetUAVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                           const UINT*);
using PSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*,
                                               UINT);
using VSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*,
                                               ID3D11ClassInstance* const*, UINT);
using CSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*,
                                               ID3D11ClassInstance* const*, UINT);
using OMSetRTs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                            ID3D11DepthStencilView*);
using OMSetRTsUAVs_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                ID3D11DepthStencilView*, UINT, UINT,
                                                ID3D11UnorderedAccessView* const*, const UINT*);
using RSSetViewports_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using Draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using Dispatch_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using Map_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using Unmap_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdateSubresource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                     const void*, UINT, UINT);
using CopyResource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopySubresourceRegion_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT,
                                                         UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using ResolveSubresource_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*,
                                                      UINT, DXGI_FORMAT);
using ClearRTV_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearDSV_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using ClearState_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using ExecuteCommandList_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishCommandList_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using SetCBs1_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                                           const UINT*);
using UpdateSubresource1_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                      const void*, UINT, UINT, UINT);

// D3D11.1 constant buffers with offsets: logged (the mod assumes plain bindings), treated as binds.
SetCBs1_t oVSSetConstantBuffers1, oPSSetConstantBuffers1;
UpdateSubresource1_t oUpdateSubresource1;

void NoteBinds1(const char* stage, UINT start, UINT count, const UINT* first, const UINT* num) {
    if (g_cbBinds1++ < 10)
        log::Info("{}SetConstantBuffers1(start {}, count {}, first constant {}, constants {}) - the engine uses D3D11.1 "
                  "constant buffer offsets",
                  stage, start, count, first ? static_cast<int>(first[0]) : -1, num ? static_cast<int>(num[0]) : -1);
}

void SetConstantBuffers(ID3D11Buffer** dst, UINT start, UINT count, ID3D11Buffer* const* buffers) {
    for (UINT i = 0; i < count; ++i)
        if (start + i < 4) dst[start + i] = buffers ? buffers[i] : nullptr;
}

template <size_t N, class T>
void SetViews(T** dst, UINT start, UINT count, T* const* views) {
    for (UINT i = 0; i < count; ++i)
        if (start + i < N) dst[start + i] = views ? views[i] : nullptr;
}

void OnSetRenderTargets(ContextState& s, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    std::memset(s.rtv, 0, sizeof(s.rtv));
    s.rtvCount = std::min<UINT>(count, 8);
    for (UINT i = 0; i < s.rtvCount; ++i) s.rtv[i] = rtvs ? rtvs[i] : nullptr;
    s.dsv = dsv;
}

template <int V>
struct ContextHooks {
    static inline SetCBs_t oVSSetConstantBuffers, oPSSetConstantBuffers, oCSSetConstantBuffers;
    static inline SetSRVs_t oPSSetShaderResources, oCSSetShaderResources;
    static inline SetUAVs_t oCSSetUnorderedAccessViews;
    static inline PSSetShader_t oPSSetShader;
    static inline VSSetShader_t oVSSetShader;
    static inline CSSetShader_t oCSSetShader;
    static inline OMSetRTs_t oOMSetRenderTargets;
    static inline OMSetRTsUAVs_t oOMSetRenderTargetsAndUAVs;
    static inline RSSetViewports_t oRSSetViewports;
    static inline Draw_t oDraw;
    static inline DrawIndexed_t oDrawIndexed;
    static inline DrawInstanced_t oDrawInstanced;
    static inline DrawIndexedInstanced_t oDrawIndexedInstanced;
    static inline Dispatch_t oDispatch;
    static inline Map_t oMap;
    static inline Unmap_t oUnmap;
    static inline UpdateSubresource_t oUpdateSubresource;
    static inline CopyResource_t oCopyResource;
    static inline CopySubresourceRegion_t oCopySubresourceRegion;
    static inline ResolveSubresource_t oResolveSubresource;
    static inline ClearRTV_t oClearRenderTargetView;
    static inline ClearDSV_t oClearDepthStencilView;
    static inline ClearState_t oClearState;
    static inline ExecuteCommandList_t oExecuteCommandList;
    static inline FinishCommandList_t oFinishCommandList;

    static void STDMETHODCALLTYPE VSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
        if (ContextState* s = StateOf(c)) SetConstantBuffers(s->vsCb, start, n, b);
        oVSSetConstantBuffers(c, start, n, b);
    }
    static void STDMETHODCALLTYPE PSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
        if (ContextState* s = StateOf(c)) SetConstantBuffers(s->psCb, start, n, b);
        oPSSetConstantBuffers(c, start, n, b);
    }
    static void STDMETHODCALLTYPE CSSetConstantBuffers(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b) {
        if (ContextState* s = StateOf(c)) SetConstantBuffers(s->csCb, start, n, b);
        oCSSetConstantBuffers(c, start, n, b);
    }
    static void STDMETHODCALLTYPE PSSetShaderResources(ID3D11DeviceContext* c, UINT start, UINT n,
                                                       ID3D11ShaderResourceView* const* v) {
        if (ContextState* s = StateOf(c)) SetViews<32>(s->psSrv, start, n, v);
        oPSSetShaderResources(c, start, n, v);
    }
    static void STDMETHODCALLTYPE CSSetShaderResources(ID3D11DeviceContext* c, UINT start, UINT n,
                                                       ID3D11ShaderResourceView* const* v) {
        if (ContextState* s = StateOf(c)) SetViews<16>(s->csSrv, start, n, v);
        oCSSetShaderResources(c, start, n, v);
    }
    static void STDMETHODCALLTYPE CSSetUnorderedAccessViews(ID3D11DeviceContext* c, UINT start, UINT n,
                                                            ID3D11UnorderedAccessView* const* v, const UINT* counts) {
        if (ContextState* s = StateOf(c)) SetViews<8>(s->csUav, start, n, v);
        oCSSetUnorderedAccessViews(c, start, n, v, counts);
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
    static void STDMETHODCALLTYPE CSSetShader(ID3D11DeviceContext* c, ID3D11ComputeShader* sh,
                                              ID3D11ClassInstance* const* ci, UINT n) {
        if (ContextState* s = StateOf(c)) s->cs = ShaderOf(sh);
        oCSSetShader(c, sh, ci, n);
    }
    static void STDMETHODCALLTYPE OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                                     ID3D11DepthStencilView* dsv) {
        if (ContextState* s = StateOf(c)) OnSetRenderTargets(*s, n, rtvs, dsv);
        oOMSetRenderTargets(c, n, rtvs, dsv);
    }
    static void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* c, UINT n,
                                                            ID3D11RenderTargetView* const* rtvs,
                                                            ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount,
                                                            ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
        if (ContextState* s = StateOf(c); s && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            OnSetRenderTargets(*s, n, rtvs, dsv);
        oOMSetRenderTargetsAndUAVs(c, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
    }
    static void STDMETHODCALLTYPE RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps) {
        if (ContextState* s = StateOf(c)) s->viewport = n && vps ? vps[0] : D3D11_VIEWPORT{};
        oRSSetViewports(c, n, vps);
    }
    static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c, UINT count, UINT start) {
        BeforeDraw(c, false, count, 1);
        oDraw(c, count, start);
        AfterDraw(c);
    }
    static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
        BeforeDraw(c, true, count, 1);
        oDrawIndexed(c, count, start, base);
        AfterDraw(c);
    }
    static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start,
                                                UINT startInstance) {
        BeforeDraw(c, false, count, instances);
        oDrawInstanced(c, count, instances, start, startInstance);
        AfterDraw(c);
    }
    static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT instances, UINT start,
                                                       INT base, UINT startInstance) {
        BeforeDraw(c, true, count, instances);
        oDrawIndexedInstanced(c, count, instances, start, base, startInstance);
        AfterDraw(c);
    }
    static void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
        if (ContextState* s = StateOf(c)) {
            if (s->cs && (s->cs->flags() & shaders::kFilter) && s->tokens.size() < 4096) s->tokens.push_back(s->cs->index);
            if (g_tracing.load(std::memory_order_relaxed)) {
                Event e;
                e.type = EventType::Dispatch;
                e.context = s->index;
                e.count = x;
                e.groupsY = y;
                e.groupsZ = z;
                e.cs = s->cs;
                FillSrvs(s->csSrv, std::size(s->csSrv), e.srvs);
                for (UINT i = 0; i < 8; ++i)
                    if (s->csUav[i]) {
                        const ViewInfo v = ViewOf(s->csUav[i]);
                        e.uavs.push_back({static_cast<uint8_t>(i), IdOf(v.resource), v.format});
                    }
                e.csCb[0] = LatestOf(*s, s->csCb[0]);
                e.csCb[1] = LatestOf(*s, s->csCb[1]);
                e.csCbId[0] = IdOf(s->csCb[0]);
                e.csCbId[1] = IdOf(s->csCb[1]);
                AddEvent(*s, std::move(e));
            }
        }
        oDispatch(c, x, y, z);
    }
    static HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                         D3D11_MAPPED_SUBRESOURCE* mapped) {
        const HRESULT hr = oMap(c, r, sub, type, flags, mapped);
        if (g_tracing.load(std::memory_order_relaxed) && SUCCEEDED(hr) && mapped && type != D3D11_MAP_READ)
            if (ContextState* s = StateOf(c)) {
                s->mapped[r] = mapped->pData;
                if (ResourceOf(r).bind & D3D11_BIND_CONSTANT_BUFFER)
                    ++(type == D3D11_MAP_WRITE_DISCARD         ? g_cbMapDiscard
                       : type == D3D11_MAP_WRITE_NO_OVERWRITE ? g_cbMapNoOverwrite
                                                              : g_cbMapOther);
            }
        return hr;
    }
    static void STDMETHODCALLTYPE Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
        if (g_tracing.load(std::memory_order_relaxed))
            if (ContextState* s = StateOf(c)) {
                if (const auto it = s->mapped.find(r); it != s->mapped.end()) {
                    const ResourceInfo info = ResourceOf(r);
                    if ((info.bind & D3D11_BIND_CONSTANT_BUFFER) && info.byteWidth && it->second) {
                        const auto* bytes = static_cast<const uint8_t*>(it->second);
                        s->latest[static_cast<ID3D11Buffer*>(r)] =
                            std::make_shared<const std::vector<uint8_t>>(bytes, bytes + info.byteWidth);
                    }
                    s->mapped.erase(it);
                }
            }
        oUnmap(c, r, sub);
    }
    static void STDMETHODCALLTYPE UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub,
                                                    const D3D11_BOX* box, const void* data, UINT rowPitch,
                                                    UINT depthPitch) {
        if (g_tracing.load(std::memory_order_relaxed) && data && !box)
            if (ContextState* s = StateOf(c)) {
                const ResourceInfo info = ResourceOf(r);
                if ((info.bind & D3D11_BIND_CONSTANT_BUFFER) && info.byteWidth) {
                    const auto* bytes = static_cast<const uint8_t*>(data);
                    s->latest[static_cast<ID3D11Buffer*>(r)] =
                        std::make_shared<const std::vector<uint8_t>>(bytes, bytes + info.byteWidth);
                    ++g_cbUpdates;
                }
            }
        oUpdateSubresource(c, r, sub, box, data, rowPitch, depthPitch);
    }
    static void STDMETHODCALLTYPE CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
        if (g_tracing.load(std::memory_order_relaxed))
            if (ContextState* s = StateOf(c)) {
                Event e;
                e.type = EventType::Copy;
                e.context = s->index;
                e.dst = IdOf(dst);
                e.src = IdOf(src);
                AddEvent(*s, std::move(e));
            }
        oCopyResource(c, dst, src);
    }
    static void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, UINT x,
                                                        UINT y, UINT z, ID3D11Resource* src, UINT srcSub,
                                                        const D3D11_BOX* box) {
        if (g_tracing.load(std::memory_order_relaxed))
            if (ContextState* s = StateOf(c)) {
                Event e;
                e.type = EventType::CopyRegion;
                e.context = s->index;
                e.dst = IdOf(dst);
                e.src = IdOf(src);
                e.text = box ? std::format("box ({},{})-({},{}) -> ({},{}) sub {}->{}", box->left, box->top,
                                           box->right, box->bottom, x, y, srcSub, dstSub)
                             : std::format("whole -> ({},{}) sub {}->{}", x, y, srcSub, dstSub);
                AddEvent(*s, std::move(e));
            }
        oCopySubresourceRegion(c, dst, dstSub, x, y, z, src, srcSub, box);
    }
    static void STDMETHODCALLTYPE ResolveSubresource(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub,
                                                     ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format) {
        if (g_tracing.load(std::memory_order_relaxed))
            if (ContextState* s = StateOf(c)) {
                Event e;
                e.type = EventType::Resolve;
                e.context = s->index;
                e.dst = IdOf(dst);
                e.src = IdOf(src);
                e.text = FormatName(format);
                AddEvent(*s, std::move(e));
            }
        oResolveSubresource(c, dst, dstSub, src, srcSub, format);
    }
    static void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv,
                                                        const FLOAT color[4]) {
        if (g_tracing.load(std::memory_order_relaxed))
            if (ContextState* s = StateOf(c)) {
                Event e;
                e.type = EventType::ClearRT;
                e.context = s->index;
                const ViewInfo v = ViewOf(rtv);
                e.dst = IdOf(v.resource);
                if (color) std::memcpy(e.clear, color, sizeof(e.clear));
                AddEvent(*s, std::move(e));
            }
        oClearRenderTargetView(c, rtv, color);
    }
    static void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags,
                                                        FLOAT depth, UINT8 stencil) {
        if (g_tracing.load(std::memory_order_relaxed))
            if (ContextState* s = StateOf(c)) {
                Event e;
                e.type = EventType::ClearDS;
                e.context = s->index;
                const ViewInfo v = ViewOf(dsv);
                e.dst = IdOf(v.resource);
                e.clear[0] = depth;
                e.clear[1] = static_cast<float>(stencil);
                e.clear[2] = static_cast<float>(flags);
                AddEvent(*s, std::move(e));
            }
        oClearDepthStencilView(c, dsv, flags, depth, stencil);
    }
    static void STDMETHODCALLTYPE ClearState(ID3D11DeviceContext* c) {
        if (ContextState* s = StateOf(c)) s->ClearBindings();
        oClearState(c);
    }
    static void STDMETHODCALLTYPE ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore) {
        CommandListRecord record;
        bool known = false;
        {
            std::lock_guard lock(g_listMutex);
            if (const auto it = g_lists.find(list); it != g_lists.end()) {
                record = std::move(it->second);
                g_lists.erase(it);
                known = true;
            }
        }
        if (known) {
            std::lock_guard lock(g_frameMutex);
            g_frameTokens.insert(g_frameTokens.end(), record.tokens.begin(), record.tokens.end());
            g_frameDraws += record.draws;
            ++g_frameLists;
            if (g_tracing.load(std::memory_order_relaxed)) {
                Event e;
                e.type = EventType::Execute;
                e.text = std::format("CL#{} recorded on deferred context {} ({} draws)", record.id, record.context,
                                     record.draws);
                e.children = std::move(record.events);
                g_frame.events.push_back(std::move(e));
            }
        }
        oExecuteCommandList(c, list, restore);
        if (!restore)
            if (ContextState* s = StateOf(c)) s->ClearBindings();
    }
    static HRESULT STDMETHODCALLTYPE FinishCommandList(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** list) {
        const HRESULT hr = oFinishCommandList(c, restore, list);
        ContextState* s = StateOf(c);
        if (s && SUCCEEDED(hr) && list && *list) {
            CommandListRecord record;
            record.id = g_nextListId++;
            record.context = s->index;
            record.draws = s->draws;
            record.tokens = std::move(s->tokens);
            record.events = std::move(s->events);
            s->tokens.clear();
            s->events.clear();
            s->draws = 0;
            s->menuDumps = 0;
            std::lock_guard lock(g_listMutex);
            g_lists[*list] = std::move(record);
            if (g_lists.size() > 256) g_lists.clear();  // lists the game never executed
        }
        if (s && !restore) s->ClearBindings();
        return hr;
    }

    static void Install(ID3D11DeviceContext* context, const char* label) {
        auto slot = [&](size_t index) { return hooks::VTableEntry(context, index); };
        hooks::Batch batch;
        const std::string p = std::string("ID3D11DeviceContext(") + label + ")::";
        hooks::Install(slot(vt::kContext_VSSetConstantBuffers), &VSSetConstantBuffers, &oVSSetConstantBuffers, (p + "VSSetConstantBuffers").c_str());
        hooks::Install(slot(vt::kContext_PSSetConstantBuffers), &PSSetConstantBuffers, &oPSSetConstantBuffers, (p + "PSSetConstantBuffers").c_str());
        hooks::Install(slot(vt::kContext_CSSetConstantBuffers), &CSSetConstantBuffers, &oCSSetConstantBuffers, (p + "CSSetConstantBuffers").c_str());
        hooks::Install(slot(vt::kContext_PSSetShaderResources), &PSSetShaderResources, &oPSSetShaderResources, (p + "PSSetShaderResources").c_str());
        hooks::Install(slot(vt::kContext_CSSetShaderResources), &CSSetShaderResources, &oCSSetShaderResources, (p + "CSSetShaderResources").c_str());
        hooks::Install(slot(vt::kContext_CSSetUnorderedAccessViews), &CSSetUnorderedAccessViews, &oCSSetUnorderedAccessViews, (p + "CSSetUnorderedAccessViews").c_str());
        hooks::Install(slot(vt::kContext_PSSetShader), &PSSetShader, &oPSSetShader, (p + "PSSetShader").c_str());
        hooks::Install(slot(vt::kContext_VSSetShader), &VSSetShader, &oVSSetShader, (p + "VSSetShader").c_str());
        hooks::Install(slot(vt::kContext_CSSetShader), &CSSetShader, &oCSSetShader, (p + "CSSetShader").c_str());
        hooks::Install(slot(vt::kContext_OMSetRenderTargets), &OMSetRenderTargets, &oOMSetRenderTargets, (p + "OMSetRenderTargets").c_str());
        hooks::Install(slot(vt::kContext_OMSetRenderTargetsAndUnorderedAccessViews), &OMSetRenderTargetsAndUAVs, &oOMSetRenderTargetsAndUAVs, (p + "OMSetRenderTargetsAndUnorderedAccessViews").c_str());
        hooks::Install(slot(vt::kContext_RSSetViewports), &RSSetViewports, &oRSSetViewports, (p + "RSSetViewports").c_str());
        hooks::Install(slot(vt::kContext_Draw), &Draw, &oDraw, (p + "Draw").c_str());
        hooks::Install(slot(vt::kContext_DrawIndexed), &DrawIndexed, &oDrawIndexed, (p + "DrawIndexed").c_str());
        hooks::Install(slot(vt::kContext_DrawInstanced), &DrawInstanced, &oDrawInstanced, (p + "DrawInstanced").c_str());
        hooks::Install(slot(vt::kContext_DrawIndexedInstanced), &DrawIndexedInstanced, &oDrawIndexedInstanced, (p + "DrawIndexedInstanced").c_str());
        hooks::Install(slot(vt::kContext_Dispatch), &Dispatch, &oDispatch, (p + "Dispatch").c_str());
        hooks::Install(slot(vt::kContext_Map), &Map, &oMap, (p + "Map").c_str());
        hooks::Install(slot(vt::kContext_Unmap), &Unmap, &oUnmap, (p + "Unmap").c_str());
        hooks::Install(slot(vt::kContext_UpdateSubresource), &UpdateSubresource, &oUpdateSubresource, (p + "UpdateSubresource").c_str());
        hooks::Install(slot(vt::kContext_CopyResource), &CopyResource, &oCopyResource, (p + "CopyResource").c_str());
        hooks::Install(slot(vt::kContext_CopySubresourceRegion), &CopySubresourceRegion, &oCopySubresourceRegion, (p + "CopySubresourceRegion").c_str());
        hooks::Install(slot(vt::kContext_ResolveSubresource), &ResolveSubresource, &oResolveSubresource, (p + "ResolveSubresource").c_str());
        hooks::Install(slot(vt::kContext_ClearRenderTargetView), &ClearRenderTargetView, &oClearRenderTargetView, (p + "ClearRenderTargetView").c_str());
        hooks::Install(slot(vt::kContext_ClearDepthStencilView), &ClearDepthStencilView, &oClearDepthStencilView, (p + "ClearDepthStencilView").c_str());
        hooks::Install(slot(vt::kContext_ClearState), &ClearState, &oClearState, (p + "ClearState").c_str());
        hooks::Install(slot(vt::kContext_ExecuteCommandList), &ExecuteCommandList, &oExecuteCommandList, (p + "ExecuteCommandList").c_str());
        hooks::Install(slot(vt::kContext_FinishCommandList), &FinishCommandList, &oFinishCommandList, (p + "FinishCommandList").c_str());
    }
};

void STDMETHODCALLTYPE hkVSSetConstantBuffers1(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b,
                                              const UINT* first, const UINT* num) {
    NoteBinds1("VS", start, n, first, num);
    if (ContextState* s = StateOf(c)) SetConstantBuffers(s->vsCb, start, n, b);
    oVSSetConstantBuffers1(c, start, n, b, first, num);
}
void STDMETHODCALLTYPE hkPSSetConstantBuffers1(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b,
                                              const UINT* first, const UINT* num) {
    NoteBinds1("PS", start, n, first, num);
    if (ContextState* s = StateOf(c)) SetConstantBuffers(s->psCb, start, n, b);
    oPSSetConstantBuffers1(c, start, n, b, first, num);
}
void STDMETHODCALLTYPE hkUpdateSubresource1(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                           const void* data, UINT rowPitch, UINT depthPitch, UINT flags) {
    if (g_tracing.load(std::memory_order_relaxed) && (ResourceOf(r).bind & D3D11_BIND_CONSTANT_BUFFER)) ++g_cbUpdates1;
    oUpdateSubresource1(c, r, sub, box, data, rowPitch, depthPitch, flags);
}

void InstallContext1Hooks(ID3D11DeviceContext* context) {
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return;
    hooks::Batch batch;
    hooks::Install(hooks::VTableEntry(context1.Get(), vt::kContext1_VSSetConstantBuffers1), &hkVSSetConstantBuffers1,
                   &oVSSetConstantBuffers1, "ID3D11DeviceContext1::VSSetConstantBuffers1");
    hooks::Install(hooks::VTableEntry(context1.Get(), vt::kContext1_PSSetConstantBuffers1), &hkPSSetConstantBuffers1,
                   &oPSSetConstantBuffers1, "ID3D11DeviceContext1::PSSetConstantBuffers1");
    hooks::Install(hooks::VTableEntry(context1.Get(), vt::kContext1_UpdateSubresource1), &hkUpdateSubresource1,
                   &oUpdateSubresource1, "ID3D11DeviceContext1::UpdateSubresource1");
}

// The context methods hooked, to compare an immediate and a deferred context's implementations.
constexpr size_t kHookedContextSlots[] = {
    vt::kContext_VSSetConstantBuffers, vt::kContext_PSSetConstantBuffers, vt::kContext_Draw, vt::kContext_DrawIndexed,
    vt::kContext_Map, vt::kContext_Unmap, vt::kContext_OMSetRenderTargets, vt::kContext_FinishCommandList};

// ---------------------------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------------------------

using CreateBuffer_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*,
                                                   ID3D11Buffer**);
using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSRV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                ID3D11ShaderResourceView**);
using CreateUAV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*,
                                                ID3D11UnorderedAccessView**);
using CreateRTV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*,
                                                ID3D11RenderTargetView**);
using CreateDSV_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*,
                                                ID3D11DepthStencilView**);
using CreateVS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11VertexShader**);
using CreatePS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11PixelShader**);
using CreateCS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11ComputeShader**);
using CreateDeferredContext_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

CreateBuffer_t oCreateBuffer;
CreateTexture2D_t oCreateTexture2D;
CreateSRV_t oCreateSRV;
CreateUAV_t oCreateUAV;
CreateRTV_t oCreateRTV;
CreateDSV_t oCreateDSV;
CreateVS_t oCreateVS;
CreatePS_t oCreatePS;
CreateCS_t oCreateCS;
CreateDeferredContext_t oCreateDeferredContext;

void RegisterView(void* view, char kind, ID3D11Resource* resource, DXGI_FORMAT format) {
    std::unique_lock lock(g_registryMutex);
    g_views[view] = {kind, resource, format};
}

HRESULT STDMETHODCALLTYPE hkCreateBuffer(ID3D11Device* d, const D3D11_BUFFER_DESC* desc,
                                         const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** out) {
    const HRESULT hr = oCreateBuffer(d, desc, data, out);
    if (SUCCEEDED(hr) && out && *out && desc && (desc->BindFlags & (D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_UNORDERED_ACCESS)))
        Register(*out, _ReturnAddress());
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateTexture2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc,
                                            const D3D11_SUBRESOURCE_DATA* data, ID3D11Texture2D** out) {
    const HRESULT hr = oCreateTexture2D(d, desc, data, out);
    if (SUCCEEDED(hr) && out && *out && desc) {
        const ResourceInfo info = Register(*out, _ReturnAddress());
        constexpr UINT kInteresting = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
        if (Get().logResources && (desc->BindFlags & kInteresting))
            log::Info("Texture R{} {}x{} {} mips={} array={} samples={} bind={} misc=0x{:x} usage={} (from {}, present {})",
                      info.id, desc->Width, desc->Height, FormatName(desc->Format), desc->MipLevels, desc->ArraySize,
                      desc->SampleDesc.Count, BindFlagsName(desc->BindFlags), desc->MiscFlags,
                      static_cast<int>(desc->Usage), info.caller, info.createdAtPresent);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateSRV(ID3D11Device* d, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                      ID3D11ShaderResourceView** out) {
    const HRESULT hr = oCreateSRV(d, r, desc, out);
    if (SUCCEEDED(hr) && out && *out) RegisterView(*out, 'S', r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateUAV(ID3D11Device* d, ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
                                      ID3D11UnorderedAccessView** out) {
    const HRESULT hr = oCreateUAV(d, r, desc, out);
    if (SUCCEEDED(hr) && out && *out) RegisterView(*out, 'U', r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateRTV(ID3D11Device* d, ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* desc,
                                      ID3D11RenderTargetView** out) {
    const HRESULT hr = oCreateRTV(d, r, desc, out);
    if (SUCCEEDED(hr) && out && *out) RegisterView(*out, 'R', r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDSV(ID3D11Device* d, ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc,
                                      ID3D11DepthStencilView** out) {
    const HRESULT hr = oCreateDSV(d, r, desc, out);
    if (SUCCEEDED(hr) && out && *out) RegisterView(*out, 'D', r, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
    return hr;
}

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

HRESULT STDMETHODCALLTYPE hkCreateCS(ID3D11Device* d, const void* code, SIZE_T size, ID3D11ClassLinkage* link,
                                     ID3D11ComputeShader** out) {
    const HRESULT hr = oCreateCS(d, code, size, link, out);
    if (SUCCEEDED(hr) && out && *out) RegisterShader(*out, shaders::Stage::kCompute, code, size);
    return hr;
}

ID3D11DeviceContext* g_firstImmediate = nullptr;

HRESULT STDMETHODCALLTYPE hkCreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out) {
    const HRESULT hr = oCreateDeferredContext(d, flags, out);
    if (SUCCEEDED(hr) && out && *out) {
        ContextState* s = RegisterContext(*out);
        log::Info("Deferred context {} created ({})", s ? s->index : -1, static_cast<void*>(*out));
        // Deferred contexts may have their own implementation of the hooked methods.
        bool differs = false;
        if (g_firstImmediate)
            for (size_t slot : kHookedContextSlots)
                differs |= hooks::VTableEntry(*out, slot) != hooks::VTableEntry(g_firstImmediate, slot);
        static std::once_flag once;
        if (differs) std::call_once(once, [&] {
            log::Info("Deferred contexts have their own method implementations: hooking them too");
            ContextHooks<1>::Install(*out, "deferred");
        });
    }
    return hr;
}

void InstallDeviceHooks(ID3D11Device* device, ID3D11DeviceContext* immediate) {
    g_device = device;
    g_immediate = immediate;
    g_firstImmediate = immediate;
    RegisterContext(immediate);
    {
        hooks::Batch batch;
        auto slot = [&](size_t index) { return hooks::VTableEntry(device, index); };
        hooks::Install(slot(vt::kDevice_CreateBuffer), &hkCreateBuffer, &oCreateBuffer, "ID3D11Device::CreateBuffer");
        hooks::Install(slot(vt::kDevice_CreateTexture2D), &hkCreateTexture2D, &oCreateTexture2D, "ID3D11Device::CreateTexture2D");
        hooks::Install(slot(vt::kDevice_CreateShaderResourceView), &hkCreateSRV, &oCreateSRV, "ID3D11Device::CreateShaderResourceView");
        hooks::Install(slot(vt::kDevice_CreateUnorderedAccessView), &hkCreateUAV, &oCreateUAV, "ID3D11Device::CreateUnorderedAccessView");
        hooks::Install(slot(vt::kDevice_CreateRenderTargetView), &hkCreateRTV, &oCreateRTV, "ID3D11Device::CreateRenderTargetView");
        hooks::Install(slot(vt::kDevice_CreateDepthStencilView), &hkCreateDSV, &oCreateDSV, "ID3D11Device::CreateDepthStencilView");
        hooks::Install(slot(vt::kDevice_CreateVertexShader), &hkCreateVS, &oCreateVS, "ID3D11Device::CreateVertexShader");
        hooks::Install(slot(vt::kDevice_CreatePixelShader), &hkCreatePS, &oCreatePS, "ID3D11Device::CreatePixelShader");
        hooks::Install(slot(vt::kDevice_CreateComputeShader), &hkCreateCS, &oCreateCS, "ID3D11Device::CreateComputeShader");
        hooks::Install(slot(vt::kDevice_CreateDeferredContext), &hkCreateDeferredContext, &oCreateDeferredContext,
                       "ID3D11Device::CreateDeferredContext");
    }
    ContextHooks<0>::Install(immediate, "immediate");
    InstallContext1Hooks(immediate);
}

// ---------------------------------------------------------------------------------------------
// D3D11CreateDevice (import of the executable)
// ---------------------------------------------------------------------------------------------

using D3D11CreateDevice_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                             UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
D3D11CreateDevice_t oD3D11CreateDevice = nullptr;

void LogAdapter(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter))) return;
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    std::string name;
    for (const wchar_t* c = desc.Description; *c; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    LARGE_INTEGER umd{};
    std::string driver = "?";
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        driver = std::format("{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                             LOWORD(umd.LowPart));
    log::Info("Adapter: {} (vendor 0x{:04x} device 0x{:04x}, {} MB VRAM, driver {}, LUID {:08x}:{:08x})", name,
              desc.VendorId, desc.DeviceId, desc.DedicatedVideoMemory >> 20, driver,
              static_cast<uint32_t>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; SUCCEEDED(adapter->EnumOutputs(i, &output)); ++i) {
        DXGI_OUTPUT_DESC od{};
        output->GetDesc(&od);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(od.Monitor, &mi);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode);
        log::Info("  Output {}: {}x{} @ {} Hz, desktop ({},{})-({},{})", i, mode.dmPelsWidth, mode.dmPelsHeight,
                  mode.dmDisplayFrequency, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                  od.DesktopCoordinates.right, od.DesktopCoordinates.bottom);
        output.Reset();
    }
}

HRESULT WINAPI hkD3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                   const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk, ID3D11Device** device,
                                   D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    proxy::EnsureInitialized();
    std::string requested;
    for (UINT i = 0; levels && i < levelCount; ++i) requested += std::format("{}{:x}", i ? "," : "", static_cast<int>(levels[i]));
    const HRESULT hr = oD3D11CreateDevice(adapter, type, software, flags, levels, levelCount, sdk, device, level, context);
    log::Info("D3D11CreateDevice(adapter={}, type={}, flags=0x{:x}, levels=[{}], sdk={}, device={}) -> 0x{:08x}",
              static_cast<void*>(adapter), static_cast<int>(type), flags, requested, sdk,
              device ? "requested" : "null (capability check)", static_cast<unsigned>(hr));
    if (SUCCEEDED(hr) && device && *device && context && *context && !g_device) {
        log::Info("Game device {} (feature level {:x}), immediate context {}", static_cast<void*>(*device),
                  static_cast<int>((*device)->GetFeatureLevel()), static_cast<void*>(*context));
        LogAdapter(*device);
        InstallDeviceHooks(*device, *context);
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// DXGI: swapchain
// ---------------------------------------------------------------------------------------------

using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
using SetFullscreenState_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using GetBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using WaitForVBlank_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*);

CreateSwapChain_t oCreateSwapChain;
Present_t oPresent;
ResizeBuffers_t oResizeBuffers;
ResizeTarget_t oResizeTarget;
SetFullscreenState_t oSetFullscreenState;
GetBuffer_t oGetBuffer;
WaitForVBlank_t oWaitForVBlank;

std::atomic<double> g_nativePresentMs{0};

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    const bool game = self == g_swapChain && !(flags & DXGI_PRESENT_TEST);
    if (game) OnPresent(self, sync, flags);
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    const HRESULT hr = oPresent(self, sync, flags);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    if (game) g_nativePresentMs = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    if (FAILED(hr)) log::Error("Present failed: 0x{:08x}", static_cast<unsigned>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                          UINT flags) {
    const HRESULT hr = oResizeBuffers(self, count, width, height, format, flags);
    log::Info("ResizeBuffers(count={}, {}x{}, {}, flags=0x{:x}) -> 0x{:08x} (present {})", count, width, height,
              FormatName(format), flags, static_cast<unsigned>(hr), g_presents.load());
    if (self == g_swapChain) g_backBuffer = nullptr;
    return hr;
}

HRESULT STDMETHODCALLTYPE hkResizeTarget(IDXGISwapChain* self, const DXGI_MODE_DESC* mode) {
    const HRESULT hr = oResizeTarget(self, mode);
    if (mode)
        log::Info("ResizeTarget({}x{} @ {}/{} Hz, {}) -> 0x{:08x}", mode->Width, mode->Height, mode->RefreshRate.Numerator,
                  mode->RefreshRate.Denominator, FormatName(mode->Format), static_cast<unsigned>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE hkSetFullscreenState(IDXGISwapChain* self, BOOL fullscreen, IDXGIOutput* target) {
    const HRESULT hr = oSetFullscreenState(self, fullscreen, target);
    log::Info("SetFullscreenState({}) -> 0x{:08x} (present {})", fullscreen ? "fullscreen" : "windowed",
              static_cast<unsigned>(hr), g_presents.load());
    return hr;
}

HRESULT STDMETHODCALLTYPE hkGetBuffer(IDXGISwapChain* self, UINT index, REFIID riid, void** surface) {
    const HRESULT hr = oGetBuffer(self, index, riid, surface);
    if (SUCCEEDED(hr) && surface && *surface && self == g_swapChain) {
        ComPtr<ID3D11Resource> resource;
        if (SUCCEEDED(static_cast<IUnknown*>(*surface)->QueryInterface(IID_PPV_ARGS(&resource)))) {
            ResourceInfo info = ResourceOf(resource.Get());
            if (resource.Get() != g_backBuffer) info = Register(resource.Get(), nullptr);  // a new buffer
            {
                std::unique_lock lock(g_registryMutex);
                g_resources[resource.Get()].backBuffer = true;
            }
            if (g_backBuffer != resource.Get()) {
                g_backBuffer = resource.Get();
                log::Info("Back buffer {}: R{} {}x{} {}", index, info.id, info.width, info.height, FormatName(info.format));
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkWaitForVBlank(IDXGIOutput* self) {
    void* caller = _ReturnAddress();
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    const HRESULT hr = oWaitForVBlank(self);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    static std::atomic<int> logged{0};
    if (logged++ < 5)
        log::Info("IDXGIOutput::WaitForVBlank from {} ({:.2f} ms, thread {})", DescribeAddress(caller),
                  static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart),
                  GetCurrentThreadId());
    return hr;
}

void OnSwapChainCreated(IDXGISwapChain* swapChain, const DXGI_SWAP_CHAIN_DESC& desc) {
    if (g_swapChain) {
        log::Info("Another swapchain {} (the game's is {}) - not tracked", static_cast<void*>(swapChain),
                  static_cast<void*>(g_swapChain));
        return;
    }
    g_swapChain = swapChain;
    log::Info("Game swapchain {}: {}x{} {} refresh {}/{} buffers={} usage=0x{:x} swapEffect={} flags=0x{:x} windowed={} "
              "window={}",
              static_cast<void*>(swapChain), desc.BufferDesc.Width, desc.BufferDesc.Height,
              FormatName(desc.BufferDesc.Format), desc.BufferDesc.RefreshRate.Numerator,
              desc.BufferDesc.RefreshRate.Denominator, desc.BufferCount, desc.BufferUsage,
              static_cast<int>(desc.SwapEffect), desc.Flags, desc.Windowed, static_cast<void*>(desc.OutputWindow));
    hooks::InstallVTable(swapChain, vt::kSwapChain_Present, &hkPresent, &oPresent, "IDXGISwapChain::Present");
    hooks::InstallVTable(swapChain, vt::kSwapChain_ResizeBuffers, &hkResizeBuffers, &oResizeBuffers,
                         "IDXGISwapChain::ResizeBuffers");
    hooks::InstallVTable(swapChain, vt::kSwapChain_ResizeTarget, &hkResizeTarget, &oResizeTarget,
                         "IDXGISwapChain::ResizeTarget");
    hooks::InstallVTable(swapChain, vt::kSwapChain_SetFullscreenState, &hkSetFullscreenState, &oSetFullscreenState,
                         "IDXGISwapChain::SetFullscreenState");
    hooks::InstallVTable(swapChain, vt::kSwapChain_GetBuffer, &hkGetBuffer, &oGetBuffer, "IDXGISwapChain::GetBuffer");

    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(swapChain->GetContainingOutput(&output)))
        hooks::Install(hooks::VTableEntry(output.Get(), vt::kOutput_WaitForVBlank), &hkWaitForVBlank, &oWaitForVBlank,
                       "IDXGIOutput::WaitForVBlank");
}

HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                            IDXGISwapChain** out) {
    const HRESULT hr = oCreateSwapChain(self, device, desc, out);
    log::Info("IDXGIFactory::CreateSwapChain -> 0x{:08x}", static_cast<unsigned>(hr));
    if (SUCCEEDED(hr) && out && *out && desc) OnSwapChainCreated(*out, *desc);
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Present: frame accounting, pass sequence, trace control
// ---------------------------------------------------------------------------------------------

std::string TokenName(uint32_t token) {
    switch (token) {
        case kTokenMenu: return "[HUD/menu]";
        case kTokenVelocity: return "[velocity]";
        case kTokenModel: return "[models]";
        case kTokenSfx: return "[effects]";
        case kTokenOther: return "[other]";
        default: {
            std::shared_lock lock(g_registryMutex);
            if (token >= 1 && token <= g_shaderStore.size()) return g_shaderStore[token - 1].name;
            return "?";
        }
    }
}

std::string DescribeTokens(const std::vector<uint32_t>& tokens) {
    std::string out;
    for (size_t i = 0; i < tokens.size();) {
        size_t j = i;
        while (j < tokens.size() && tokens[j] == tokens[i]) ++j;
        out += (out.empty() ? "" : " > ") + TokenName(tokens[i]);
        if (j - i > 1) out += std::format(" x{}", j - i);
        i = j;
    }
    return out;
}

std::vector<uint32_t> g_lastTokens;
uint64_t g_lastSequenceLog = 0;

bool KeyPressed(int vk) {
    static bool down[256]{};
    if (vk <= 0 || vk >= 256) return false;
    const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = now && !down[vk];
    down[vk] = now;
    return pressed;
}

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void StartTrace() {
    if (g_traceState != TraceState::Idle) return;
    g_traceState = TraceState::Warmup;
    log::Info("Frame trace armed at present {}: recording the next {} frames", g_presents.load(), Get().traceFrames);
}

void FinishTrace() {
    g_tracing = false;
    g_traceState = TraceState::Idle;
    TraceData data;
    {
        std::lock_guard lock(g_frameMutex);
        data.frames = std::move(g_tracedFrames);
        g_tracedFrames.clear();
    }
    {
        std::shared_lock lock(g_registryMutex);
        for (const auto& [resource, info] : g_resources) data.resources[info.id] = info;
        if (g_backBuffer)
            if (const auto it = g_resources.find(g_backBuffer); it != g_resources.end()) data.backBuffer = it->second.id;
    }
    for (size_t i = 0; i < g_contextCount.load(); ++i) {
        g_contextStates[i].latest.clear();
        g_contextStates[i].mapped.clear();
    }
    const uint64_t first = data.frames.empty() ? 0 : data.frames.front().present;
    data.file = OutputDirectory() / std::format("DSRProbe_trace_{}.txt", first);
    if (dump::Pending() && g_immediate)
        dump::Flush(g_immediate, OutputDirectory() / "DSRProbe_dumps" / std::format("trace_{}", first));
    log::Info("Frame trace finished: writing {}", data.file.string());
    std::thread([d = std::move(data)] {
        WriteTrace(d);
        log::Info("Frame trace written: {}", d.file.string());
    }).detach();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------------------------

void InstallEarlyHooks() {
    hooks::PatchImport(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDevice", &hkD3D11CreateDevice,
                       &oD3D11CreateDevice);
}

void OnFactoryCreated(IUnknown* factory) {
    ComPtr<IDXGIFactory> f;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f))))
        hooks::Install(hooks::VTableEntry(f.Get(), vt::kFactory_CreateSwapChain), &hkCreateSwapChain, &oCreateSwapChain,
                       "IDXGIFactory::CreateSwapChain");
    static std::atomic<int> count{0};
    if (count++ < 8) log::Debug("DXGI factory {} created", static_cast<void*>(factory));
}

uint64_t PresentCount() { return g_presents.load(); }

bool Tracing() { return g_tracing.load(); }

void TraceNote(std::string text) {
    if (!g_tracing) return;
    Event e;
    e.type = EventType::Note;
    e.text = std::move(text);
    AppendImmediate(std::move(e));
}

void OnPresent(IDXGISwapChain*, UINT syncInterval, UINT flags) {
    const uint64_t present = ++g_presents;

    static UINT lastSync = ~0u;
    if (syncInterval != lastSync) {
        lastSync = syncInterval;
        log::Info("Present: sync interval {} flags 0x{:x} (present {})", syncInterval, flags, present);
    }

    // The frame's passes (immediate context draws + executed command lists).
    std::vector<uint32_t> tokens;
    uint32_t draws = 0, lists = 0;
    if (ContextState* s = StateOf(g_immediate)) {
        std::lock_guard lock(g_frameMutex);
        g_frameTokens.insert(g_frameTokens.begin(), s->tokens.begin(), s->tokens.end());
        s->tokens.clear();
        draws = g_frameDraws + s->draws;
        s->draws = 0;
        s->menuDumps = 0;
        lists = g_frameLists;
        tokens = std::move(g_frameTokens);
        g_frameTokens.clear();
        g_frameDraws = g_frameLists = 0;
    }
    // Logged when the frame's structure changes: its named passes, and whether it has velocity / HUD
    // draws (not the number of model / effect batches, which varies every frame).
    std::vector<uint32_t> signature;
    for (uint32_t t : tokens)
        if (t < 0x80000000u || t == kTokenVelocity || t == kTokenMenu)
            if (signature.empty() || signature.back() != t) signature.push_back(t);
    if (signature != g_lastTokens && present - g_lastSequenceLog > 30) {
        g_lastSequenceLog = present;
        g_lastTokens = std::move(signature);
        log::Info("Frame {} passes ({} draws, {} command lists): {}", present, draws, lists, DescribeTokens(tokens));
    }
    static uint64_t lastStats = 0;
    if (present - lastStats >= 600) {
        lastStats = present;
        log::Info("Present {}: {} draws, {} command lists, native Present {:.2f} ms", present, draws, lists,
                  g_nativePresentMs.load());
    }

    // Trace state machine: arm -> one warm-up frame (command lists recorded before arming are
    // incomplete) -> record N frames -> write.
    const bool focus = GameHasFocus();
    const bool traceKey = focus && KeyPressed(Get().traceKey);
    const bool flipperKey = focus && KeyPressed(Get().flipperKey);
    const bool snapshotKey = focus && KeyPressed(Get().snapshotKey);
    if (traceKey || (Get().traceAtFrame > 0 && present == static_cast<uint64_t>(Get().traceAtFrame))) StartTrace();

    // Textures are dumped during the last traced frame only: the first one may miss command lists
    // that worker threads were already recording when the trace was armed.
    const int dumpBudget = Get().dumpTextures ? Get().maxDumps : 0;
    switch (g_traceState.load()) {
        case TraceState::Warmup:
            g_traceFramesLeft = Get().traceFrames;
            g_dumpsLeft = g_traceFramesLeft == 1 ? dumpBudget : 0;
            {
                std::lock_guard lock(g_frameMutex);
                g_frame = {};
                g_frame.present = present + 1;
            }
            g_tracing = true;
            g_traceState = TraceState::Recording;
            break;
        case TraceState::Recording: {
            // The back buffer as presented, at the last traced frame's Present.
            if (dumpBudget && g_backBuffer && g_traceFramesLeft == 1 && g_immediate)
                dump::Capture(g_immediate, g_backBuffer, "backbuffer_at_present");
            {
                std::lock_guard lock(g_frameMutex);
                Event e;
                e.type = EventType::Note;
                e.text = std::format("Present(sync {}, flags 0x{:x}). Constant buffer updates this frame: Map "
                                     "WRITE_DISCARD {}, Map NO_OVERWRITE {}, other Map {}, UpdateSubresource {}, "
                                     "UpdateSubresource1 {}; *SetConstantBuffers1 calls so far {}",
                                     syncInterval, flags, g_cbMapDiscard.exchange(0), g_cbMapNoOverwrite.exchange(0),
                                     g_cbMapOther.exchange(0), g_cbUpdates.exchange(0), g_cbUpdates1.exchange(0),
                                     g_cbBinds1.load());
                g_frame.events.push_back(std::move(e));
                g_tracedFrames.push_back(std::move(g_frame));
                g_frame = {};
                g_frame.present = present + 1;
            }
            --g_traceFramesLeft;
            g_dumpsLeft = g_traceFramesLeft == 1 ? dumpBudget : 0;
            if (g_traceFramesLeft <= 0) FinishTrace();
            break;
        }
        default: break;
    }

    TimingOnPresent(present);
    MemoryOnPresent(present, flipperKey, snapshotKey);
}

}  // namespace dsru::probe
