# Build and install obs-input-logger to the per-user OBS plugin directory.
#
# Usage (from a PowerShell prompt in the repo root):
#     .\install-windows.ps1                    # VS 2022 (default preset)
#     .\install-windows.ps1 -Preset vs2026     # Visual Studio 2026
#
# What it does:
#   1. cmake configure using the chosen preset
#   2. cmake build (RelWithDebInfo)
#   3. Copy the resulting .dll + data/ into
#      %APPDATA%\obs-studio\plugins\obs-input-logger\
#
# Requires: cmake 3.28+ and either VS 2022 or VS 2026 (with "Desktop C++ workload")
# already installed. No admin rights needed — installs to your user profile.

param(
    [ValidateSet('vs2022', 'vs2026')]
    [string]$Preset = 'vs2022'
)

$ErrorActionPreference = 'Stop'

$cfgPreset = if ($Preset -eq 'vs2026') { 'windows-x64-vs2026' } else { 'windows-x64' }
$buildPreset = $cfgPreset

Write-Host "==> Configuring ($cfgPreset)..."
cmake --preset $cfgPreset
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "==> Building..."
cmake --build --preset $buildPreset --config RelWithDebInfo
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$destRoot = Join-Path $env:APPDATA 'obs-studio\plugins\obs-input-logger'
$destBin  = Join-Path $destRoot 'bin\64bit'
$destData = Join-Path $destRoot 'data'

Write-Host "==> Installing to $destRoot ..."
New-Item -ItemType Directory -Force -Path $destBin  | Out-Null
New-Item -ItemType Directory -Force -Path $destData | Out-Null

# Locate the built DLL (build_x64\RelWithDebInfo\obs-input-logger.dll or similar).
$dll = Get-ChildItem -Path 'build_x64' -Recurse -Filter 'obs-input-logger.dll' |
       Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' } |
       Select-Object -First 1
if (-not $dll) { throw "Could not find obs-input-logger.dll under build_x64\" }

Copy-Item -Force $dll.FullName (Join-Path $destBin 'obs-input-logger.dll')

# Optional PDB for crash debugging.
$pdb = Get-ChildItem -Path 'build_x64' -Recurse -Filter 'obs-input-logger.pdb' -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' } |
       Select-Object -First 1
if ($pdb) { Copy-Item -Force $pdb.FullName (Join-Path $destBin 'obs-input-logger.pdb') }

# Copy locale data.
$locale = Join-Path $PSScriptRoot 'data\locale'
if (Test-Path $locale) {
    Copy-Item -Recurse -Force $locale (Join-Path $destData 'locale')
}

Write-Host ""
Write-Host "OK. Installed to: $destRoot"
Write-Host "Restart OBS; Tools menu should show 'Input Logger: Enabled'."
