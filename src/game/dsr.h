#pragma once

// Dark Souls Remastered specific knowledge. Everything version-dependent is found at runtime (RTTI,
// shader ids from the game's own archives), never hardcoded addresses.
//
// Executable (DarkSoulsRemastered.exe 1.0.0.0, PE timestamp 0x6344ca56, Oct 2022):
//  - imports d3d11!D3D11CreateDevice and dxgi!CreateDXGIFactory directly (a dxgi.dll proxy loads, and
//    d3d11.dll binds its own dxgi import, CreateDXGIFactory2, to it)
//  - D3D11 feature level 11.0, no debug flags; the swapchain comes from IDXGIFactory::CreateSwapChain
//    (legacy description): B8G8R8A8_UNORM, 2 buffers, DXGI_SWAP_EFFECT_DISCARD, then
//    SetFullscreenState(TRUE) in fullscreen mode. Eight deferred contexts record from worker threads.
//  - protected against code changes (a second, obfuscated .text section, some functions only jump into
//    it): import-table hooks, COM hooks and data writes (vtables, step tables) are safe; code patches
//    only once the game has run for a few seconds.
//  - RTTI kept: NS_FRPG::FrpgFlipper_* frame pacing classes (game/flipper.h), ImageProcess* post
//    processing classes (ImageProcessResolveTAA, ImageProcessToneMap, ...).
//  - debug strings: "Antialiasing mode: Off / FXAA / FXAA High / Temporal", "FlipMode 30FPS / 60FPS /
//    140FPS": the engine has TAA and a 140 FPS pacing mode.
//
// Shaders (shader/*_DX11.shaderbnd.dcx: DXBC, SM 5.0, named, see game/shader_db.h). The engine
// emulates DX9 constant registers: every shader has one $Globals buffer (b0), vertex constants
// gVC_* at fixed registers, pixel constants DL_FREG_nnn.
//  - Models: FRPG_{Phn,Gst,Wind,Ivy,Inst_*}_<vertex format>_<pass>: passes Dep (depth), Sdw (shadow),
//    Vel / VelAlp (object velocity), main color (Hem*). gVC_WorldViewClipMtx (c0-c3) is the
//    view-projection (jittered under TAA), gVC_LocalWorldMtxArray the bones / world matrices.
//  - Velocity (FRPG_*_Vel.vpo): o = WorldViewClip * p, TEXCOORD0 = gVC_CommonREG12 (c12-c15) * p
//    (this frame, unjittered), TEXCOORD1 = gVC_CommonREG8 (c8-c11) * prev p (previous frame);
//    FRPG_Phn_Dif________________Vel.fpo writes (curNDC - prevNDC) / 2 (y up) to an RG target whose
//    clear value (x >= 1) means "no object motion".
//  - TAA resolve FRPG_Fil_ResolveTAA.fpo (full-screen, VS FRPG_Fil_*.vpo from SV_VertexID):
//        t0 scene color (HDR)   t1 history   t2 depth (0 near .. 1 far, standard Z)
//        t3 velocity            t4 previous frame's o1 (sign = moving)
//        o0 resolved color, o1.xy = (1 / motion length, linear depth: negative where moving)
//        cb0[8]  depth linearization: linear = w / (z * depth + y)  (y = far, w = near * far)
//        cb0[12] screen size (w, h, 1/w, 1/h)
//        cb0[56] x: velocity weight ("Temporal velocity weight" in the debug menu), cb0[57] motion scale
//        cb0[58..61] clip -> world, cb0[74..77] world -> previous clip, cb0[78] camera offset
//                    between the frames (the engine renders camera relative)
//    motion = object velocity (x, -y) where present, else uv - reproject(depth): prevUV = uv - motion.
//  - Post processing after the resolve: motion blur (FRPG_Fil_MotionBlur*), depth of field, bloom,
//    tone mapping FRPG_Fil_HDR[_PBL][_ColAdj], optional FXAA (FRPG_Fil_FxAA2[_High]); the HUD and menus
//    use FRPG_Menu_* shaders.
//
// Seen with DSRProbe (RTX 5090, 3840x2160, Sep 2026):
//  - Options > Anti-Aliasing: OFF / FXAA / FXAA High / Temporal Anti-Aliasing. Only the last runs the
//    TAA resolve (and the velocity pass jitters); the velocity pass runs in every mode (motion blur).
//  - 11 deferred contexts; the whole post-processing chain and the HUD are recorded on one of them.
//    The HUD is drawn into the tonemapped scene target (B8G8R8A8), then a final pass of the game's own
//    (pixel shader 4915b254c739f466, not in the archives: brightness / gamma) writes the back buffer on
//    the immediate context.
//  - Constant buffers: one per shader kind, Map(WRITE_DISCARD) before every draw (~9000 per frame),
//    bound with VS/PSSetConstantBuffers1 (null offsets). Depth: D24_UNORM_S8_UINT.
//  - Frame rate: 60 FPS from a busy-wait on QueryPerformanceCounter against an engine clock whose rate
//    is a data field (game/frame_timer.h). The FrpgFlipper_140FPS class swap alone changes nothing (the
//    engine sets the owner's mode back every frame): the clock's rate sets the frame rate (game/high_fps.h).
namespace dsru::game {

// Logs the executable's name, version and PE timestamp; warns if it isn't DarkSoulsRemastered.exe.
void LogGameInfo();

// Standard depth (1 = far): the upscalers and frame generation get depthInverted = false.
inline constexpr bool kDepthInverted = false;

}  // namespace dsru::game
