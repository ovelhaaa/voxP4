# PowerShell environment verification script for ESP-IDF v5.3 on Windows
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$IdfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { "$HOME\esp\esp-idf" }

Write-Host "=== VoxP4 ESP-IDF Environment Check (Windows PowerShell) ==="
Write-Host "Repository root: $Root"
Write-Host "IDF_PATH:        $IdfPath"

if (-not (Test-Path "$IdfPath\export.ps1")) {
    Write-Error "A valid ESP-IDF checkout was not found at $IdfPath."
    exit 1
}

# Source ESP-IDF environment
& "$IdfPath\export.ps1" | Out-Null

$IdfVersion = & python "$IdfPath\tools\idf.py" --version
$PythonVersion = & python --version
$CmakeVersion = (& cmake --version | Select-Object -First 1)
$NinjaVersion = (& ninja --version | Select-Object -First 1)
$GccVersion = (& riscv32-esp-elf-gcc --version | Select-Object -First 1)

Write-Host "ESP-IDF Version:   $IdfVersion"
Write-Host "Python Version:    $PythonVersion"
Write-Host "CMake Version:     $CmakeVersion"
Write-Host "Ninja Version:     $NinjaVersion"
Write-Host "RISC-V Toolchain:  $GccVersion"
Write-Host "Target:            esp32p4"
Write-Host "=== Environment is Ready ==="
