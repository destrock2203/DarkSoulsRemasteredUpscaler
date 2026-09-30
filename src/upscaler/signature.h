#pragma once

#include <filesystem>

namespace dsru::upscaler {

// True when `file` carries a valid Authenticode signature chaining to a trusted root (vendor
// runtimes are only loaded when signed). With `signer`, the signing certificate's name must also
// contain it (e.g. L"NVIDIA").
bool VerifySignature(const std::filesystem::path& file, const wchar_t* signer = nullptr);

// LoadLibrary of a vendor runtime from the mod's plugin folder (Game/DSRUpscaler/), after its
// signature check. Null (logged) when missing, failing to load, or unsigned with
// [Loader] AllowUnsignedDlls=0.
void* LoadSignedModule(const char* feature, const wchar_t* fileName);

}  // namespace dsru::upscaler
