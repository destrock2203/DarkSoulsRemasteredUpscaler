# Undoes install.ps1: restores every saved original and deletes the files the mods added.
# DSR 2020 Textures are packed by DSR-TPUP, which keeps its own backups (its Unpack/Restore).
param(
    [string]$GameDir = "F:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED"
)
$ErrorActionPreference = "Stop"

$Backup = Join-Path $GameDir "_modlist_backup"
if (-not (Test-Path $Backup)) { Write-Host "Nothing to undo"; exit }

$Created = Join-Path $Backup "created.txt"
foreach ($rel in Get-Content $Created) {
    $path = Join-Path $GameDir $rel
    if (Test-Path $path) { Remove-Item $path -Force }
}

$saved = Get-ChildItem $Backup -Recurse -File | Where-Object { $_.FullName -ne $Created }
foreach ($f in $saved) {
    Copy-Item $f.FullName (Join-Path $GameDir $f.FullName.Substring($Backup.Length).TrimStart('\')) -Force
}

Remove-Item $Backup -Recurse -Force
Write-Host ("Restored {0} files" -f $saved.Count)
