# Installs the mod list into a clean Dark Souls Remastered folder.
# Every game file replaced is first saved under <game>\_modlist_backup (once: the first copy is the
# vanilla one), files the mods add are listed in _modlist_backup\created.txt; uninstall.ps1 undoes both.
param(
    [string]$GameDir = "F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED",
    [string]$ModDir = "F:\Mod\dsmod\ds1 mod new"
)
$ErrorActionPreference = "Stop"

$Backup = Join-Path $GameDir "_modlist_backup"
$Created = Join-Path $Backup "created.txt"
New-Item -ItemType Directory -Force $Backup | Out-Null
if (-not (Test-Path $Created)) { New-Item -ItemType File $Created | Out-Null }

function Save-Original([string]$rel) {
    $dst = Join-Path $GameDir $rel
    $bak = Join-Path $Backup $rel
    if (Test-Path $dst) {
        if (-not (Test-Path $bak)) {
            New-Item -ItemType Directory -Force (Split-Path $bak) | Out-Null
            Copy-Item $dst $bak
        }
    } elseif (-not (Select-String -Path $Created -SimpleMatch -Pattern $rel -Quiet)) {
        Add-Content -Path $Created -Value $rel
    }
}

function Install-File([string]$src, [string]$rel) {
    Save-Original $rel
    $dst = Join-Path $GameDir $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item $src $dst -Force
}

# Copies every file under $src to $rel (a folder relative to the game).
function Install-Tree([string]$src, [string]$rel) {
    if (-not (Test-Path $src)) { throw "Missing: $src" }
    $files = Get-ChildItem $src -Recurse -File
    foreach ($f in $files) {
        Install-File $f.FullName (Join-Path $rel $f.FullName.Substring($src.Length).TrimStart('\'))
    }
    Write-Host ("  {0} files -> {1}" -f $files.Count, $(if ($rel) { $rel } else { "<game>" }))
}

# Sets key=value in an INI file (the key must already be there).
function Set-IniValue([string]$rel, [string]$key, [string]$value) {
    Save-Original $rel
    $path = Join-Path $GameDir $rel
    $lines = Get-Content $path
    $found = $false
    $lines = $lines | ForEach-Object {
        if ($_ -match "^\s*$([regex]::Escape($key))\s*=") { $found = $true; "$key=$value" } else { $_ }
    }
    if (-not $found) { throw "$key not found in $rel" }
    Set-Content -Path $path -Value $lines -Encoding ASCII
    Write-Host "  $rel : $key=$value"
}

Write-Host "1. DSR 2020 Textures (220): skipped, packed into the game archives with DSR-TPUP (see README)"

Write-Host "2. Restored Lighting 2.3.0 (1423)"
$rl = Join-Path $ModDir "DSRRL Complete 1423 2.3.0 2026-10-18T05-56Z U5qNMgvR9\DSRRL Complete package v2.3.0"
Install-Tree (Join-Path $rl "param") "param"
Install-Tree (Join-Path $rl "DSRRL") "DSRRL"
Install-File (Join-Path $rl "DSRRL_Restored_Lighting_v2.3.0.addon64") "DSRRL_Restored_Lighting_v2.3.0.addon64"
# Its dxgi.dll is a ReShade build (DSRRL Runtime): chain-loaded by DSRUpscaler's dxgi.dll.
Install-File (Join-Path $rl "dxgi.dll") "ReShade64.dll"
Write-Host "  DSRRL Runtime -> ReShade64.dll"

Write-Host "3. Better Bonfire Menu 1.0.1"
$bb = Join-Path $ModDir "01 - Better Bonfire Menu v1.0.1"
foreach ($d in "event", "map", "msg", "script") { Install-Tree (Join-Path $bb $d) $d }

Write-Host "4. Compact HUD and Better UI Scaling (1071)"
Install-Tree (Join-Path $ModDir "Compact HUD and Better UI Scaling-1071-1-0-1757506018\menu") "menu"

# The resonance effects are always on the player (40 = none, re-applied every second; 41-44 =
# levels 1-4, 4 s): each adds 2131 when it expires, so a death always finds it, also after a reload.
Write-Host "5. No death penalty: SpEffectParam 40-44 (resonance) replaceSpEffectId -> 2131 (Rare Ring of Sacrifice)"
$gp = "param\GameParam\GameParam.parambnd.dcx"
Save-Original $gp
python -I (Join-Path $PSScriptRoot "tools\dsr_param.py") (Join-Path $GameDir $gp) SpEffectParam 40,41,42,43,44 292 2131 --expect -1
if ($LASTEXITCODE -ne 0) { throw "param patch failed" }

Write-Host "6. DS1 FPS Unlock 1.9.0 (zofrea1, MIT)"
$fu = Join-Path $ModDir "DS1-FPS-Unlock-v1.9.0-REMASTERED-DX11"
Install-File (Join-Path $fu "dinput8.dll") "dinput8.dll"
Install-File (Join-Path $fu "DSR-FPS-Unlock.ini") "DSR-FPS-Unlock.ini"
Set-IniValue "DSR-FPS-Unlock.ini" "MaxFPS" "90"

Write-Host "7. DSRUpscaler + ReShade chain"
if (-not (Test-Path (Join-Path $GameDir "dxgi.dll"))) { throw "DSRUpscaler's dxgi.dll is not in the game folder" }
Set-IniValue "DSRUpscaler.ini" "ChainLoadDXGI" "ReShade64.dll"
# DSRUpscaler sets DS1 FPS Unlock's limit live: from the refresh rate and frame generation.
Set-IniValue "DSRUpscaler.ini" "AutoFPS" "1"
if (Test-Path (Join-Path $GameDir "ReShade.ini")) {
    Set-IniValue "ReShade.ini" "PresetPath" ".\ReShadePreset.ini"
    # End: Home is DSRUpscaler's menu.
    Set-IniValue "ReShade.ini" "KeyOverlay" "35,0,0,0"
}

Write-Host "Done. Backups: $Backup"
