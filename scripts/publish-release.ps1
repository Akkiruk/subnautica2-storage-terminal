# Builds, packages, and uploads a StorageTerminal release to CurseForge.
#
# ---------------------------------------------------------------------------
# THE TOKEN IS NEVER STORED IN THIS REPO.
#
# It is read from the CURSEFORGE_TOKEN environment variable and sent only as an
# X-Api-Token header to the configured CurseForge host. It is never written to
# disk, never echoed, never put in a URL query string (which would land it in
# server logs and shell history), and never included in an error message.
#
# Set it for your user account once:
#   [Environment]::SetEnvironmentVariable("CURSEFORGE_TOKEN", "<token>", "User")
# then open a new shell. Verify with:  $env:CURSEFORGE_TOKEN.Length
# ---------------------------------------------------------------------------
#
# Usage:
#   .\scripts\publish-release.ps1 -WhatIf              # build+package, no upload
#   .\scripts\publish-release.ps1 -CheckAuth           # verify token only
#   .\scripts\publish-release.ps1 -ProjectId 123456
#   .\scripts\publish-release.ps1 -ProjectId 123456 -ReleaseType beta

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    # CurseForge numeric project id. Find it on the project page sidebar.
    [string]$ProjectId = $env:CURSEFORGE_PROJECT_ID,

    # The CurseForge host for this game. Upload URIs are relative to the site
    # the project lives on, so this differs per game and must match the project.
    [string]$ApiHost = $(if ($env:CURSEFORGE_HOST) { $env:CURSEFORGE_HOST } else { "https://subnautica2.curseforge.com" }),

    [ValidateSet("release", "beta", "alpha")]
    [string]$ReleaseType = "release",

    # Game version ids to tag the file with (from /api/game/versions).
    [int[]]$GameVersionIds = @(),

    # Skip the build and reuse whatever DLL is already built.
    [switch]$NoBuild,

    # Only validate the token and exit.
    [switch]$CheckAuth
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$ue4ssSrc = Join-Path $projectRoot "vendor\RE-UE4SS-src"
$dllPath = Join-Path $ue4ssSrc "build_vs\Game__Shipping__Win64\bin\StorageTerminal.dll"
$distDir = Join-Path $projectRoot "dist"

# --- token -----------------------------------------------------------------
$token = $env:CURSEFORGE_TOKEN
if ([string]::IsNullOrWhiteSpace($token)) {
    throw @"
CURSEFORGE_TOKEN is not set.

Set it for your user account (do NOT put it in a file in this repo):
  [Environment]::SetEnvironmentVariable("CURSEFORGE_TOKEN", "<token>", "User")
then open a new shell.
"@
}
$headers = @{ "X-Api-Token" = $token }

# --- auth check ------------------------------------------------------------
function Test-CurseForgeAuth {
    $uri = "$ApiHost/api/game/versions"
    try {
        $versions = Invoke-RestMethod -Uri $uri -Headers $headers -Method Get
        Write-Host "Auth OK against $ApiHost ($($versions.Count) game versions visible)."
        return $versions
    } catch {
        # Deliberately does not echo the token or the full request.
        throw "Auth/endpoint check failed for $uri : $($_.Exception.Message)`nCheck CURSEFORGE_TOKEN and that -ApiHost matches the game this project is on."
    }
}

if ($CheckAuth) {
    $versions = Test-CurseForgeAuth
    $versions | Select-Object -First 25 id, name, slug | Format-Table
    Write-Host ""
    Write-Host "Pass the ids you want as -GameVersionIds 1234,5678"
    return
}

# --- version consistency ---------------------------------------------------
$versionFile = Join-Path $projectRoot "VERSION"
$version = (Get-Content $versionFile -Raw).Trim()
if ($version -notmatch "^\d+\.\d+\.\d+$") { throw "VERSION contains '$version', not MAJOR.MINOR.PATCH." }

$cpp = Get-Content (Join-Path $projectRoot "src\main.cpp") -Raw
if ($cpp -notmatch 'ModVersion = STR\("(\d+\.\d+\.\d+)"\);') {
    throw "Could not find ModVersion in src/main.cpp."
}
$cppVersion = $Matches[1]
if ($cppVersion -ne $version) {
    throw "Version drift: VERSION says $version but src/main.cpp ModVersion says $cppVersion. Run scripts\bump-version.ps1 instead of editing by hand."
}

# --- changelog -------------------------------------------------------------
$changelogPath = Join-Path $projectRoot "CHANGELOG.md"
if (-not (Test-Path $changelogPath)) { throw "CHANGELOG.md not found." }
$changelogRaw = Get-Content $changelogPath -Raw

# Take just this version's section -- from its heading to the next one.
$sectionPattern = "(?sm)^##\s+" + [regex]::Escape($version) + "\s*(?:-[^\r\n]*)?\r?\n(.*?)(?=^##\s|\z)"
$match = [regex]::Match($changelogRaw, $sectionPattern)
if (-not $match.Success) {
    throw "CHANGELOG.md has no '## $version' section. Add release notes before publishing."
}
$changelog = $match.Groups[1].Value.Trim()
if ([string]::IsNullOrWhiteSpace($changelog) -or $changelog -match "Describe the change here") {
    throw "The '## $version' changelog section is still the generated stub. Write real release notes first."
}

Write-Host "Publishing StorageTerminal $version ($ReleaseType)"

# --- build -----------------------------------------------------------------
if (-not $NoBuild) {
    $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars" }
    Write-Host "Building..."
    cmd.exe /c "`"$vcvars`" x64 && set PATH=%PATH%;C:\Program Files\CMake\bin && cd /d `"$ue4ssSrc`" && cmake --build build_vs --config Game__Shipping__Win64 --target StorageTerminal"
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}
if (-not (Test-Path $dllPath)) { throw "Built DLL not found at $dllPath" }

# --- package -------------------------------------------------------------
# Layout mirrors the live mod folder exactly, so the zip extracts straight into
# Subnautica2/Binaries/Win64/Mods/.
# NOTE: every local file operation below passes -WhatIf:$false explicitly.
# SupportsShouldProcess makes -WhatIf cascade into every cmdlet in the script,
# which skipped the staging directory and then failed at Compress-Archive. Here
# -WhatIf is meant to gate the UPLOAD only -- building and packaging locally is
# side-effect-free as far as anyone else is concerned, and being able to inspect
# the zip without publishing is the whole point of a dry run.
New-Item -ItemType Directory -Path $distDir -Force -WhatIf:$false | Out-Null
$stage = Join-Path $distDir "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage -WhatIf:$false }
$dllTarget = Join-Path $stage "StorageTerminal\dlls"
New-Item -ItemType Directory -Path $dllTarget -Force -WhatIf:$false | Out-Null
Copy-Item $dllPath (Join-Path $dllTarget "StorageTerminal.dll") -WhatIf:$false

$install = @"
StorageTerminal $version

INSTALL
  1. Install UE4SS for Subnautica 2.
  2. Copy the StorageTerminal folder next to this file into:
       Subnautica2\Binaries\Win64\Mods\
  3. Add this line to Subnautica2\Binaries\Win64\Mods\mods.txt
     (above the "Keybinds : 1" line):
       StorageTerminal : 1

USE
  Walk up to any NoA computer terminal and pick "Access storage network".
  Type to search. Page Up / Page Down move between lockers holding the item.
  Escape closes.

This mod is read-only: it never moves, creates, or deletes items.
"@
Set-Content -Path (Join-Path $stage "README.txt") -Value $install -Encoding utf8 -WhatIf:$false

$zipName = "StorageTerminal-$version.zip"
$zipPath = Join-Path $distDir $zipName
if (Test-Path $zipPath) { Remove-Item -Force $zipPath -WhatIf:$false }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipPath -WhatIf:$false
Remove-Item -Recurse -Force $stage -WhatIf:$false
Write-Host "Packaged $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1KB, 1)) KB)"

# --- upload ---------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($ProjectId)) {
    Write-Warning "No -ProjectId given (and CURSEFORGE_PROJECT_ID is unset), so nothing was uploaded."
    Write-Host "The package is ready at: $zipPath"
    return
}

$metadata = @{
    changelog     = $changelog
    changelogType = "markdown"
    displayName   = "StorageTerminal $version"
    releaseType   = $ReleaseType
}
if ($GameVersionIds.Count -gt 0) { $metadata.gameVersions = $GameVersionIds }
$metadataJson = $metadata | ConvertTo-Json -Compress -Depth 5

$uploadUri = "$ApiHost/api/projects/$ProjectId/upload-file"

if (-not $PSCmdlet.ShouldProcess($uploadUri, "Upload StorageTerminal $version")) {
    Write-Host "-WhatIf: built and packaged only. Would have uploaded to $uploadUri"
    return
}

Write-Host "Uploading to $uploadUri ..."
$form = @{
    metadata = $metadataJson
    file     = Get-Item $zipPath
}
try {
    $response = Invoke-RestMethod -Uri $uploadUri -Headers $headers -Method Post -Form $form
} catch {
    throw "Upload failed: $($_.Exception.Message)"
}

Write-Host "Uploaded. CurseForge file id: $($response.id)"
Write-Host "It will appear once CurseForge finishes processing/approval."
