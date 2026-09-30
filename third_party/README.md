# Third-party SDKs

Vendor SDKs are not committed (size and licensing). CMake looks for them here first, then in the
Elden Ring mod's `../EldenRingUpscaler/third_party/` (same layout, same SDKs). Override with
`-DDSRU_STREAMLINE_DIR=...`, `-DDSRU_FIDELITYFX_DIR=...`, `-DDSRU_XESS_DIR=...`.

| Feature | SDK | Expected layout | Download |
|---|---|---|---|
| DLSS (upscaling, Frame Generation, Reflex) | NVIDIA Streamline 2.x | `streamline/include/sl.h`, `streamline/bin/x64/` | https://github.com/NVIDIAGameWorks/Streamline/releases |
| FSR (upscaling, frame generation) | AMD FidelityFX SDK 2.x (`Kits/FidelityFX`) | `fidelityfx/api/include/ffx_api.h`, `fidelityfx/signedbin/` | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| XeSS, XeSS-FG, XeLL | Intel XeSS SDK 2.x / 3.x | `xess/inc/xess/xess_d3d12.h`, `xess/bin/libxess.dll` | https://github.com/intel/xess/releases |

The runtime DLLs are copied to `build/<preset>/DSRUpscaler/` and ship in
`DARK SOULS REMASTERED/DSRUpscaler/`. They are signature-checked before loading.
