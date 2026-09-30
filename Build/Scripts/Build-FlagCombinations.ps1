# Builds the plugin once per O3D_WITH_TRANSPORT_* combination (WP-F2 acceptance, TRB-24, TRF-27).
#
# Each combination runs Build-Plugin.ps1 (RunUAT BuildPlugin, Win64) with the listed environment
# variables set to 0 and every other O3D_WITH_TRANSPORT_* variable unset. The default build (all
# transports on) is not repeated here; PR CI and the nightly already build it.
#
# Every combination runs even when an earlier one fails. The script exits 1 if any failed.
# Each BuildPlugin run takes about as long as the normal plugin build, so this runs in the
# nightly workflow (and by hand), not on every PR.
param(
  [Parameter(Mandatory = $true)][string]$UEPath,
  [Parameter(Mandatory = $true)][string]$PluginUPluginPath,
  # One package folder per combination is created under this directory.
  [Parameter(Mandatory = $true)][string]$OutRoot,
  [string]$Configuration = "Development",
  # Names of the combinations to build (see $Combinations below). Default: all of them.
  [string[]]$Only = @(),
  # Passed through to Build-Plugin.ps1.
  [switch]$StrictIncludes,
  [switch]$FailOnWarnings
)

# O3D_WITH_TRANSPORT_WEBRTC is not here: WebRTC is the Open3DBroadcastWebRTC add-on plugin, which
# reads that flag in its own Build.cs (WP-F11).
$TransportFlags = @(
  "O3D_WITH_TRANSPORT_SOCKETS",
  "O3D_WITH_TRANSPORT_NNG",
  "O3D_WITH_TRANSPORT_MOQ"
)

# Each transport off on its own, then all of them off (only Loopback left).
$Combinations = [ordered]@{
  "no-sockets"    = @("O3D_WITH_TRANSPORT_SOCKETS")
  "no-nng"        = @("O3D_WITH_TRANSPORT_NNG")
  "no-moq"        = @("O3D_WITH_TRANSPORT_MOQ")
  "loopback-only" = $TransportFlags
}

$names = @($Combinations.Keys)
if ($Only.Count -gt 0) {
  $unknown = @($Only | Where-Object { $names -notcontains $_ })
  if ($unknown.Count -gt 0) {
    Write-Host "::error::Unknown combination(s): $($unknown -join ', '). Known: $($names -join ', ')."
    exit 2
  }
  $names = @($Only)
}

$buildPlugin = Join-Path $PSScriptRoot "Build-Plugin.ps1"
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

# Keep the caller's values so they can be put back afterwards.
$saved = @{}
foreach ($flag in $TransportFlags) {
  $saved[$flag] = [Environment]::GetEnvironmentVariable($flag)
}

$results = [ordered]@{}
try {
  foreach ($name in $names) {
    foreach ($flag in $TransportFlags) {
      Remove-Item -Path "Env:$flag" -ErrorAction SilentlyContinue
    }
    foreach ($flag in $Combinations[$name]) {
      Set-Item -Path "Env:$flag" -Value "0"
    }

    $off = $Combinations[$name] -join ", "
    Write-Host ""
    Write-Host "=== Combination '$name': $off = 0 ==="

    $buildArgs = @{
      UEPath            = $UEPath
      PluginUPluginPath = $PluginUPluginPath
      OutDir            = (Join-Path $OutRoot $name)
      TargetPlatforms   = @("Win64")
      Configuration     = $Configuration
      StrictIncludes    = $StrictIncludes
      FailOnWarnings    = $FailOnWarnings
    }

    $exitCode = 1
    try {
      & $buildPlugin @buildArgs
      $exitCode = $LASTEXITCODE
      if ($null -eq $exitCode) { $exitCode = 1 }
    } catch {
      Write-Host "::error::Combination '$name' threw: $($_.Exception.Message)"
      $exitCode = 1
    }

    $results[$name] = $exitCode
    if ($exitCode -ne 0) {
      Write-Host "::error::Plugin build failed with $off = 0 (combination '$name', exit code $exitCode)."
    }
  }
} finally {
  foreach ($flag in $TransportFlags) {
    if ($null -ne $saved[$flag]) {
      Set-Item -Path "Env:$flag" -Value $saved[$flag]
    } else {
      Remove-Item -Path "Env:$flag" -ErrorAction SilentlyContinue
    }
  }
}

Write-Host ""
Write-Host "Flag-combination builds:"
$failed = 0
foreach ($name in $results.Keys) {
  $status = "passed"
  if ($results[$name] -ne 0) {
    $status = "FAILED"
    $failed++
  }
  Write-Host ("  {0,-14} {1}" -f $name, $status)
}

if ($failed -gt 0) {
  exit 1
}
exit 0
