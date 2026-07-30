# Copies the built StorageTerminal.dll into the live UE4SS mod folder.
#
# The mod is built as a cppmod INSIDE the UE4SS source tree
# (vendor/RE-UE4SS-src/cppmods/StorageTerminal), which reaches back into
# ../src -- so the build output lives under the UE4SS build directory, not
# under this project. There is no standalone CMake project; a stale one used
# to sit at the repo root pointing at paths that never existed, and it also
# omitted InventoryEventHook.cpp from its source list, so it could not have
# linked even if anyone had used it.
#
# The live mod root is Binaries/Win64/Mods -- NOT the nested
# Binaries/Win64/ue4ss/Mods, which contains only a leftover mods.txt. The
# loader reads the former; check UE4SS.log's "Starting mods (from mods.txt
# ...)" line if that ever changes.

param(
    [string]$GameRoot = "C:\SteamLibrary\steamapps\common\Subnautica2",
    [switch]$Build,
    [switch]$KillGame
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$ue4ssSrc = Join-Path $projectRoot "vendor\RE-UE4SS-src"
$dllPath = Join-Path $ue4ssSrc "build_vs\Game__Shipping__Win64\bin\StorageTerminal.dll"
$destDir = Join-Path $GameRoot "Subnautica2\Binaries\Win64\Mods\StorageTerminal\dlls"

if ($Build) {
    $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars" }
    cmd.exe /c "`"$vcvars`" x64 && set PATH=%PATH%;C:\Program Files\CMake\bin && cd /d `"$ue4ssSrc`" && cmake --build build_vs --config Game__Shipping__Win64 --target StorageTerminal"
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}

# The game holds the DLL open, so a redeploy needs it closed.
if ($KillGame) {
    if (Get-Process -Name "Subnautica2-Win64-Shipping" -ErrorAction SilentlyContinue) {
        taskkill /F /IM Subnautica2-Win64-Shipping.exe /T | Out-Null
        Start-Sleep -Seconds 2
    }
}

if (-not (Test-Path $dllPath)) {
    throw "Built DLL not found at $dllPath. Run with -Build, or build the StorageTerminal target in $ue4ssSrc\build_vs."
}

New-Item -ItemType Directory -Path $destDir -Force | Out-Null
Copy-Item -Path $dllPath -Destination (Join-Path $destDir "StorageTerminal.dll") -Force

Write-Host "Installed StorageTerminal.dll to $destDir"
Write-Host "Ensure 'StorageTerminal : 1' is present in $GameRoot\Subnautica2\Binaries\Win64\Mods\mods.txt"
