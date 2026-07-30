# Bumps the mod version in every place it appears, so they cannot drift.
#
# Sources of truth touched:
#   VERSION            the canonical number, read by publish-release.ps1
#   src/main.cpp       ModVersion, which is what UE4SS.log reports
#   CHANGELOG.md       a new section stub for the release notes
#
# Usage:
#   .\scripts\bump-version.ps1 -Part patch        # 1.0.0 -> 1.0.1
#   .\scripts\bump-version.ps1 -Part minor        # 1.0.1 -> 1.1.0
#   .\scripts\bump-version.ps1 -Part major        # 1.1.0 -> 2.0.0
#   .\scripts\bump-version.ps1 -Version 1.4.2     # set explicitly

[CmdletBinding(DefaultParameterSetName = "Part")]
param(
    [Parameter(ParameterSetName = "Part")]
    [ValidateSet("major", "minor", "patch")]
    [string]$Part = "patch",

    [Parameter(ParameterSetName = "Explicit")]
    [ValidatePattern("^\d+\.\d+\.\d+$")]
    [string]$Version
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$versionFile = Join-Path $projectRoot "VERSION"
$mainCpp = Join-Path $projectRoot "src\main.cpp"
$changelog = Join-Path $projectRoot "CHANGELOG.md"

if (-not (Test-Path $versionFile)) { throw "VERSION not found at $versionFile" }
if (-not (Test-Path $mainCpp)) { throw "src/main.cpp not found at $mainCpp" }

$current = (Get-Content $versionFile -Raw).Trim()
if ($current -notmatch "^\d+\.\d+\.\d+$") {
    throw "VERSION contains '$current', which is not a MAJOR.MINOR.PATCH version."
}

if ($PSCmdlet.ParameterSetName -eq "Explicit") {
    $new = $Version
} else {
    $parts = $current.Split(".") | ForEach-Object { [int]$_ }
    switch ($Part) {
        "major" { $new = "$($parts[0] + 1).0.0" }
        "minor" { $new = "$($parts[0]).$($parts[1] + 1).0" }
        "patch" { $new = "$($parts[0]).$($parts[1]).$($parts[2] + 1)" }
    }
}

if ($new -eq $current) { throw "New version equals current version ($current); nothing to do." }

# --- VERSION ---------------------------------------------------------------
Set-Content -Path $versionFile -Value $new -NoNewline:$false -Encoding utf8

# --- src/main.cpp ----------------------------------------------------------
# Anchored on the ModVersion assignment specifically so nothing else that looks
# like a version string can be caught by accident.
$cpp = Get-Content $mainCpp -Raw
$pattern = 'ModVersion = STR\("(\d+\.\d+\.\d+)"\);'
$found = [regex]::Matches($cpp, $pattern)
if ($found.Count -ne 1) {
    throw "Expected exactly one ModVersion assignment in src/main.cpp, found $($found.Count). Fix by hand."
}
$cpp = [regex]::Replace($cpp, $pattern, "ModVersion = STR(`"$new`");")
Set-Content -Path $mainCpp -Value $cpp -Encoding utf8 -NoNewline

# --- CHANGELOG.md ----------------------------------------------------------
$today = (Get-Date).ToString("yyyy-MM-dd")
$stub = @"
## $new - $today

- Describe the change here. This text is what gets uploaded to CurseForge as
  the release changelog, so write it for players.


"@

if (Test-Path $changelog) {
    $existing = Get-Content $changelog -Raw
    # Insert directly beneath the title so newest is always first.
    if ($existing -match "(?s)^(# [^\r\n]*\r?\n\r?\n)(.*)$") {
        Set-Content -Path $changelog -Value ($Matches[1] + $stub + $Matches[2]) -Encoding utf8
    } else {
        Set-Content -Path $changelog -Value ($stub + $existing) -Encoding utf8
    }
} else {
    Set-Content -Path $changelog -Value ("# Changelog`r`n`r`n" + $stub) -Encoding utf8
}

Write-Host "Bumped $current -> $new"
Write-Host "  VERSION"
Write-Host "  src/main.cpp (ModVersion)"
Write-Host "  CHANGELOG.md (new section stub -- edit it before publishing)"
Write-Host ""
Write-Host "Next: edit CHANGELOG.md, then .\scripts\publish-release.ps1"
