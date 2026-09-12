# PowerShell build script for ESP32-P4 on Windows
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$IdfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { "$HOME\esp\esp-idf" }

Set-Location $Root

# Source ESP-IDF environment
& "$IdfPath\export.ps1" | Out-Null

Write-Host "Building VoxP4 for ESP32-P4..."
& idf.py build

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LASTEXITCODE."
    exit $LASTEXITCODE
}

Write-Host "Build complete: build\vox_p4.bin"
