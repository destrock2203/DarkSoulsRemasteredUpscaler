# Mod list

The mod list played with DSRUpscaler. The mods live in `F:\Mod\dsmod\ds1 mod new` (not in git);
this folder holds the install order and the scripts. Start from a clean game (Steam: verify files).

| # | Mod | Game files | How |
|---|---|---|---|
| 1 | [DSR 2020 Textures](https://www.nexusmods.com/darksoulsremastered/mods/220) | textures in `map`, `chr`, `obj` archives | DSR-TPUP, by hand (below) |
| 2 | [Restored Lighting 2.3.0](https://www.nexusmods.com/darksoulsremastered/mods/1423) | `param/DrawParam`, `DSRRL/`, its addon, its ReShade build as `ReShade64.dll` | `install.ps1` |
| 3 | Better Bonfire Menu 1.0.1 | `event`, `map/MapStudio`, `msg`, `script/talk` | `install.ps1` |
| 4 | [Compact HUD and Better UI Scaling](https://www.nexusmods.com/darksoulsremastered/mods/1071) | `menu/menu.drb.dcx` | `install.ps1` |
| 5 | No death penalty | `GameParam`: SpEffectParam 40-44 (resonance, always on the player) `replaceSpEffectId` -1 -> 2131 (Rare Ring of Sacrifice) | `install.ps1` (`tools/dsr_param.py`) |
| 6 | [DS1 FPS Unlock 1.9.0](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered) (MIT) | `dinput8.dll`, `DSR-FPS-Unlock.ini` (`MaxFPS=90`) | `install.ps1` |
| 7 | DSRUpscaler | `dxgi.dll`, chain-loads `ReShade64.dll` (`ChainLoadDXGI`); sets 6's limit live (`AutoFPS=1`: refresh rate / frame generation) | build, copy by hand |

No two mods write the same file, and no texture is in both 1 and 2 (2 replaces textures at run time
through its ReShade addon).

## Install

```powershell
modlist\install.ps1      # 2 to 7; replaced files saved in <game>\_modlist_backup
modlist\uninstall.ps1    # restores them, deletes what the mods added
```

DSR 2020 Textures: get [DSR Texture Packer and Unpacker](https://www.nexusmods.com/darksoulsremastered/mods/36),
copy the mod's `map`, `chr` and `obj` folders into its `Texture Override` folder, set the game folder,
**Repack**. Its **Restore** undoes it.
