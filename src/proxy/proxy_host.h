#pragma once

#include <string>

struct IUnknown;

// What the dxgi.dll proxy (proxy/dxgi_proxy.cpp) needs from the DLL it is built into: the mod
// (DSRUpscaler) and the probe (DSRProbe) are both dxgi.dll and implement these differently.
namespace dsru::proxy {

// One-time initialization, from the first proxied export call (never under the loader lock).
void EnsureInitialized();

// Another dxgi.dll to forward to instead of the system one (ReShade renamed), relative to the
// module's folder; empty = the system dxgi.dll.
std::wstring ChainLoadPath();

// Every DXGI factory the game (or d3d11.dll on its behalf) creates.
void OnFactoryCreated(IUnknown* factory);

}  // namespace dsru::proxy
