#pragma once

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <vector>

// The game's D3D11 device and contexts. D3D11CreateDevice is caught through the executable's import
// table; the device's shader creation and the contexts' methods are hooked (the immediate context and
// the deferred contexts the engine records on from worker threads). Each context's pipeline state is
// tracked as far as the mod needs it, and the draws that matter are handed to render/upscale.h, found
// by the role of their shaders (game/shader_db.h):
//     TAA resolve  pixel shader FRPG_Fil_ResolveTAA: inputs t0 color, t2 depth, t3 velocity
//     velocity     vertex shaders *_Vel / *_VelAlp: the jittered and unjittered view-projection
//     HUD          FRPG_Menu_* pixel shaders: the scene is complete (HUD-less) before the first one
//     gamma        the game's final pass (a shader of its own, not from the archives): scene + HUD with the
//                  brightness setting, into the back buffer
namespace dsru::hooks::d3d11 {

enum class Role : uint8_t { None, TaaResolve, Velocity, Hud, Gamma };

struct ShaderInfo {
    uint64_t id = 0;
    Role role = Role::None;
};

// Constant buffer contents captured when the game unmaps / updates them (the buffers are dynamic:
// they can't be read back). Only for the draws with a role (see the .cpp).
struct CapturedConstants {
    ID3D11Buffer* source = nullptr;  // the buffer they were written to
    std::vector<uint8_t> data;
};

struct ContextState {
    ID3D11DeviceContext* context = nullptr;
    bool deferred = false;
    uint8_t index = 0;  // 0 = immediate
    ShaderInfo vs, ps;
    ID3D11ShaderResourceView* psSrv[8]{};
    ID3D11RenderTargetView* rtv[8]{};
    UINT rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Buffer* vsCb0 = nullptr;
    ID3D11Buffer* psCb0 = nullptr;
    CapturedConstants vsConstants, psConstants;
    // A Map of a buffer to capture, until its Unmap.
    ID3D11Resource* mapped = nullptr;
    void* mappedData = nullptr;
    bool mappedForPs = false;
    // HUD already seen in the command list / frame being recorded on this context.
    bool hudSeen = false;
    // Deferred contexts: splits and callbacks recorded in the current command list (SplitHere, AtExecute).
    struct Split {
        ID3D11CommandList* prefix;  // null: `atExecute` runs after the whole list (AtExecute)
        std::function<void()> atExecute;
    };
    std::vector<Split> splits;
    // After a split the command list is a new one: D3D11 refuses Map(NO_OVERWRITE) on a deferred
    // context until the resource was mapped with DISCARD in the current list (the game would get a null
    // pointer). Resources re-discarded since the last split.
    bool splitActive = false;
    std::vector<ID3D11Resource*> discarded;
    // HUD redirect (upscaling): while set, render target `redirectFrom` is replaced by `redirectTo`, and
    // the viewport / scissor the game sets while it is bound are scaled to its size.
    ID3D11RenderTargetView* redirectFrom = nullptr;
    ID3D11RenderTargetView* redirectTo = nullptr;
    float redirectScaleX = 1, redirectScaleY = 1;
    D3D11_VIEWPORT viewport{};  // the game's, unscaled
    bool hasViewport = false;
    D3D11_RECT scissor{};  // the game's, unscaled
    bool hasScissor = false;
};

// From the D3D11CreateDevice import hook: the executable's calls.
void InstallEarlyHooks();

// The game's device, once created (null before).
ID3D11Device* Device();
ID3D11DeviceContext* Immediate();

// `state` is the context the draw is recorded on. Splits a deferred context's command list here:
// everything recorded so far executes first, then `atExecute` runs on the immediate context's thread
// (with the immediate context), then the rest of the list. On the immediate context `atExecute` runs
// right away.
void SplitHere(ContextState& state, std::function<void()> atExecute);

// Runs `fn` on the immediate context's thread once the command list being recorded on `state` has been
// executed (right away on the immediate context): frame bookkeeping in GPU submission order.
void AtExecute(ContextState& state, std::function<void()> fn);

// From here to the end of the command list being recorded (or EndRedirect): the game's render target
// `from` becomes `to` (another size), its viewports / scissors are scaled by (scaleX, scaleY) while `to`
// is bound. Applied to the current bindings right away.
void BeginRedirect(ContextState& state, ID3D11RenderTargetView* from, ID3D11RenderTargetView* to, float scaleX,
                   float scaleY);
void EndRedirect(ContextState& state);
// The immediate context's state (present thread).
ContextState* ImmediateState();

}  // namespace dsru::hooks::d3d11
