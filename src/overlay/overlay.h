#pragma once

#include <d3d11.h>

// In-game overlay (Dear ImGui, Win32 + DX11 backends), drawn into the frame's back buffer on the
// immediate context right before it is presented:
//  - a status line in the top-left corner (upscaler, frame generation, FPS), unless turned off
//  - the settings window, toggled with [Overlay] MenuKey (Home)
// While the window is open, the game's mouse / keyboard input (window messages, cursor calls and
// DirectInput devices) is held back.
namespace dsru::overlay {

void Render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width, UINT height, HWND window);

// Around a resize of the back buffer.
void OnBeforeResize();

}  // namespace dsru::overlay
