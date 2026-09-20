# Rebuild and capture bounded B1 policies on an attached ESP32-P4.
# Restores the caller's sdkconfig even if one policy fails.
param(
  [string]$Port = 'COM11',
  [int[]]$Policies = @(0, 1, 2, 4, 8, 16),
  [string]$OutputDirectory = 'artifacts/alpha01d'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$original = [IO.File]::ReadAllText((Join-Path $root 'sdkconfig'))
$python = Join-Path $HOME '.espressif/python_env/idf5.3_py3.14_env/Scripts/python.exe'
try {
  foreach ($policy in $Policies) {
    $current = [IO.File]::ReadAllText((Join-Path $root 'sdkconfig'))
    $updated = [regex]::Replace($current,
      'CONFIG_VOXP4_PSOLA_B1_NEWEST_LIMIT=\d+',
      "CONFIG_VOXP4_PSOLA_B1_NEWEST_LIMIT=$policy")
    if ($updated -eq $current -and $policy -ne [int]([regex]::Match($current,
        'CONFIG_VOXP4_PSOLA_B1_NEWEST_LIMIT=(\d+)').Groups[1].Value)) {
      throw 'B1 policy setting is absent from sdkconfig'
    }
    [IO.File]::WriteAllText((Join-Path $root 'sdkconfig'), $updated)
    & "$PSScriptRoot/build-p4.ps1" *> "$OutputDirectory/psola_b1_policy${policy}_build.log"
    if ($LASTEXITCODE -ne 0) { throw "Build failed for policy $policy" }
    & $python "$PSScriptRoot/run_psola_prediction_device.py" --port $Port `
      --output "$OutputDirectory/psola_b1_policy${policy}_raw.txt" --timeout 360 `
      *> "$OutputDirectory/psola_b1_policy${policy}_run.log"
    if ($LASTEXITCODE -ne 0) { throw "Capture failed for policy $policy" }
    Write-Host "B1_POLICY_DONE policy=$policy"
  }
} finally {
  [IO.File]::WriteAllText((Join-Path $root 'sdkconfig'), $original)
}
