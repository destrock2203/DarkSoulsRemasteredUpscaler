DSRUpscaler
===========

DLSS / FSR / XeSS upscaling and frame generation for Dark Souls Remastered, sharpening, optional
DLSS 5 neural rendering on RTX cards, and an in-game menu.

ONLINE PLAY
-----------
This mod injects dxgi.dll and changes how the game is rendered and presented. Online play with it is
untested and at your own risk; playing offline is recommended.

INSTALL
-------
Extract the archive into the DARK SOULS REMASTERED folder (next to DarkSoulsRemastered.exe). You
should end up with:
  DARK SOULS REMASTERED\dxgi.dll
  DARK SOULS REMASTERED\DSRUpscaler.ini
  DARK SOULS REMASTERED\DSRUpscaler\...   (runtime DLLs and licenses)

In the game's options (PC Settings > Graphics), set Anti-Aliasing to TXAA: the mod replaces the
game's TAA, and without it there is nothing to replace (the menu says so).

Press Home in game for the menu. Every setting changed in the menu is saved to DSRUpscaler.ini.
Keys: Home = menu   F7 = upscaler on/off   F8 = DLSS 5 on/off

Any display mode works. In fullscreen the mod runs the game as a borderless window covering the
monitor, so frame generation, switching DLSS / FSR / XeSS in game, overlays and ReShade behave the
same in every mode. Set the game's resolution below the monitor's to upscale (the menu shows which
resolution gives which quality mode); at the monitor's resolution the upscaler runs as native AA
(DLAA).

UNINSTALL
---------
Delete dxgi.dll, DSRUpscaler.ini, the DSRUpscaler*.log files and the DSRUpscaler folder.

WHAT WORKS ON WHICH GPU
-----------------------
  DLSS upscaling / DLAA ...... NVIDIA RTX
  DLSS frame generation ...... NVIDIA RTX 40 series and newer
  FSR / FSR frame generation . any GPU
  XeSS / XeSS-FG ............. Intel Arc, or any GPU with Shader Model 6.4
  Sharpening ................. any GPU (AMD RCAS, NVIDIA Image Scaling, Laplacian)
  DLSS 5 (optional) .......... NVIDIA RTX 50 series, runtime not included (below)

Anything your system lacks simply shows as unavailable in the menu. The mod keeps running if a
runtime DLL is missing; the reason is written to DSRUpscaler.log.

FRAME RATE
----------
The game runs at its own 60 FPS. To unlock it, install DS1 FPS Unlock by zofrea1
(github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered), not included: dinput8.dll and
DSR-FPS-Unlock.ini next to the game. It keeps the game's speed at any frame rate, and the mod's menu
then sets the frame rate limit live (Frame rate section), by default just below your monitor's
refresh rate divided by the frame generation multiplier.

DLSS 5 (OPTIONAL)
-----------------
The mod supports DLSS 5 neural rendering. The DLSS 5 runtime (nvngx_dlssnr.dll) is NOT included
and is not distributed by this mod. If you have it, place it in DARK SOULS REMASTERED\DSRUpscaler\
and enable DLSS 5 in the menu or with F8 (with or without the upscaler). Without it, DLSS 5 shows as
unavailable and everything else works normally.

RESHADE
-------
ReShade also uses dxgi.dll, so the mod loads it for you:
  1. Install ReShade for DarkSoulsRemastered.exe (DirectX 10/11/12).
  2. Rename ReShade's dxgi.dll to ReShade64.dll.
  3. Copy this mod's dxgi.dll into the game folder again (ReShade's installer replaced it).
  4. In DSRUpscaler.ini, set:  ChainLoadDXGI=ReShade64.dll
  5. ReShade opens its menu with Home too: change its Overlay key (ReShade menu > Settings),
     e.g. to End.
Updating or reinstalling ReShade replaces dxgi.dll again: redo steps 2 and 3.

TROUBLESHOOTING
---------------
DSRUpscaler.log, in the game folder, explains what loaded and what did not. Include it when
reporting a problem. Overlays that hook the swapchain (RivaTuner/RTSS, etc.) are the first thing to
try disabling.

CREDITS AND LICENSES
--------------------
NVIDIA DLSS, DLSS Frame Generation, Reflex, Image Scaling and Streamline (c) NVIDIA Corporation.
AMD FidelityFX (FSR, RCAS) (c) Advanced Micro Devices, Inc. RCAS is adapted from FSR 1 (MIT).
Intel XeSS, XeSS-FG and XeLL (c) Intel Corporation.
MinHook (c) Tsuda Kageyu. Dear ImGui (c) Omar Cornut.
The license texts are in DARK SOULS REMASTERED\DSRUpscaler\licenses. The NVIDIA, AMD and Intel
components are provided under their vendors' licenses: using them means accepting those terms,
including the ban on reverse engineering, modifying or redistributing them on their own.
DLSS 5 integration approach after OptiScaler_DLSSNR (github.com/Dagherbou/OptiScaler_DLSSNR).
Frame rate unlock: DS1 FPS Unlock by zofrea1 (MIT), installed separately.
This mod is not affiliated with, sponsored or endorsed by NVIDIA, AMD, Intel, FromSoftware or
Bandai Namco.
