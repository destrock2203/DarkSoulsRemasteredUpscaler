# Builds the public release zip: build\package\DSRUpscaler-v<version>.zip, laid out like the
# DARK SOULS REMASTERED folder it is extracted into.
# Usage: powershell -ExecutionPolicy Bypass -File package.ps1 [-SkipBuild]
#
# Only the files listed below are packaged. The DLSS 5 runtime (nvngx_dlssnr*.dll) is NVIDIA driver
# content without a redistribution license: it is never shipped, users add it themselves. Neither is
# DS1 FPS Unlock (installed separately) nor anything DSRProbe dumps from the game.
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not $SkipBuild) {
    & cmd.exe /c "`"$root\build.bat`" x64-release"
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$version = [regex]::Match((Get-Content "$root\CMakeLists.txt" -Raw), 'VERSION\s+(\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw "project version not found in CMakeLists.txt" }
$build = "$root\build\x64-release"
$runtime = "$build\DSRUpscaler"
$name = "DSRUpscaler-v$version"
$stage = "$root\build\package\$name"
$zip = "$root\build\package\$name.zip"

# Destination (relative to the game folder) -> source.
$files = [ordered]@{
    'dxgi.dll'                                       = "$build\dxgi.dll"
    'DSRUpscaler.ini'                                = "$root\dist\DSRUpscaler.ini"
    'DSRUpscaler_README.txt'                         = "$root\dist\README.txt"
    'DSRUpscaler\dsru_nvngx.dll'                     = "$runtime\dsru_nvngx.dll"
    # NVIDIA Streamline + DLSS / DLSS-G / Reflex / NIS
    'DSRUpscaler\sl.interposer.dll'                  = "$runtime\sl.interposer.dll"
    'DSRUpscaler\sl.common.dll'                      = "$runtime\sl.common.dll"
    'DSRUpscaler\sl.dlss.dll'                        = "$runtime\sl.dlss.dll"
    'DSRUpscaler\sl.dlss_g.dll'                      = "$runtime\sl.dlss_g.dll"
    'DSRUpscaler\sl.reflex.dll'                      = "$runtime\sl.reflex.dll"
    'DSRUpscaler\sl.pcl.dll'                         = "$runtime\sl.pcl.dll"
    'DSRUpscaler\sl.nis.dll'                         = "$runtime\sl.nis.dll"
    'DSRUpscaler\nvngx_dlss.dll'                     = "$runtime\nvngx_dlss.dll"
    'DSRUpscaler\nvngx_dlssg.dll'                    = "$runtime\nvngx_dlssg.dll"
    # AMD FidelityFX (FSR, FSR frame generation)
    'DSRUpscaler\amd_fidelityfx_loader_dx12.dll'     = "$runtime\amd_fidelityfx_loader_dx12.dll"
    'DSRUpscaler\amd_fidelityfx_upscaler_dx12.dll'   = "$runtime\amd_fidelityfx_upscaler_dx12.dll"
    'DSRUpscaler\amd_fidelityfx_framegeneration_dx12.dll' = "$runtime\amd_fidelityfx_framegeneration_dx12.dll"
    # Intel XeSS, XeSS-FG, XeLL
    'DSRUpscaler\libxess.dll'                        = "$runtime\libxess.dll"
    'DSRUpscaler\libxess_fg.dll'                     = "$runtime\libxess_fg.dll"
    'DSRUpscaler\libxell.dll'                        = "$runtime\libxell.dll"
    # Licenses the vendors require next to their binaries (copied by the build)
    'DSRUpscaler\licenses\Streamline-license.txt'    = "$runtime\licenses\Streamline-license.txt"
    'DSRUpscaler\licenses\NVIDIA-DLSS-license.txt'   = "$runtime\licenses\NVIDIA-DLSS-license.txt"
    'DSRUpscaler\licenses\NVIDIA-Reflex-license.txt' = "$runtime\licenses\NVIDIA-Reflex-license.txt"
    'DSRUpscaler\licenses\NVIDIA-NIS-license.txt'    = "$runtime\licenses\NVIDIA-NIS-license.txt"
    'DSRUpscaler\licenses\AMD-FidelityFX-license.md' = "$runtime\licenses\AMD-FidelityFX-license.md"
    'DSRUpscaler\licenses\Intel-XeSS-license.txt'    = "$runtime\licenses\Intel-XeSS-license.txt"
    'DSRUpscaler\licenses\Intel-XeSS-third-party-programs.txt' = "$runtime\licenses\Intel-XeSS-third-party-programs.txt"
    # Libraries compiled into dxgi.dll
    'DSRUpscaler\licenses\MinHook-license.txt'       = "$runtime\licenses\MinHook-license.txt"
    'DSRUpscaler\licenses\DearImGui-license.txt'     = "$runtime\licenses\DearImGui-license.txt"
    'DSRUpscaler\licenses\AMD-FSR1-RCAS-license.txt' = "$runtime\licenses\AMD-FSR1-RCAS-license.txt"
}

$missing = @($files.GetEnumerator() | Where-Object { -not (Test-Path $_.Value) } | ForEach-Object { $_.Value })
if ($missing) { throw "missing files:`n  " + ($missing -join "`n  ") }

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
if (Test-Path $zip) { Remove-Item $zip -Force }
foreach ($f in $files.GetEnumerator()) {
    $dest = Join-Path $stage $f.Key
    New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
    Copy-Item $f.Value $dest
}

# Safety net: nothing from the DLSS 5 runtime or DS1 FPS Unlock, no logs, no debug symbols, no dumps.
$forbidden = Get-ChildItem $stage -Recurse -File |
    Where-Object { $_.Name -match 'dlssnr|dinput8|FPS-Unlock|DSRProbe|\.log$|\.pdb$|\.dmp$' }
if ($forbidden) { throw "forbidden files staged: " + ($forbidden.Name -join ', ') }

# Entries written by hand: Windows PowerShell's Compress-Archive stores '\' separators, which some
# mod managers and non-Windows unzip tools extract as flat file names.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    foreach ($file in Get-ChildItem $stage -Recurse -File) {
        $entry = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Write-Host "Packaged $zip"
