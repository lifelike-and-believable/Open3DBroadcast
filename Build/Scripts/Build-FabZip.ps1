param(
  [string]$UEPath,
  # The Fab source zip made by Build/Scripts/fab-package.py, or a folder that contains it.
  [string]$Zip,
  # Scratch folder: the zip is extracted to <WorkDir>\Stage and packaged to <WorkDir>\Package.
  [string]$WorkDir,
  # Before BuildPlugin, check that the zip holds everything it needs to build on its own:
  # the Open3DStreamCore module and its core mirror, and no leftover prebuilt core
  # (WP-F1). CI passes it. BuildPlugin would fail without these files anyway; the check
  # says why in one line.
  [switch]$RequireStandalone
)

# Runs RunUAT BuildPlugin on the contents of the Fab source zip, the way Fab's build
# farm would, then checks the resulting binaries (CI-5).
#
# Nothing is added to the extracted zip: since WP-F1 the o3ds core is compiled from the
# mirror in Source\ThirdParty\Open3DStreamCore by the Open3DStreamCore module, so the zip
# builds exactly as Fab's farm receives it.
#
# Exit codes: 0 built and checked; 1 BuildPlugin failed, a warning was reported in plugin
# sources, or a check failed; 2 bad arguments.

function Write-Failure([string]$Message) { Write-Host "::error::$Message" }

$UEPath = $UEPath.Trim('"', "'")
$Zip = $Zip.Trim('"', "'")
$WorkDir = $WorkDir.Trim('"', "'")

if (Test-Path -LiteralPath $Zip -PathType Container) {
  $found = @(Get-ChildItem -LiteralPath $Zip -Filter *.zip -File)
  if ($found.Count -ne 1) {
    Write-Failure "Expected exactly one .zip in $Zip, found $($found.Count)."
    exit 2
  }
  $Zip = $found[0].FullName
}
if (!(Test-Path -LiteralPath $Zip -PathType Leaf)) {
  Write-Failure "Fab zip not found: $Zip"
  exit 2
}

$stage = Join-Path $WorkDir "Stage"
$package = Join-Path $WorkDir "Package"
foreach ($d in @($stage, $package)) {
  if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Expand-Archive -LiteralPath $Zip -DestinationPath $stage

$descriptors = @(Get-ChildItem -LiteralPath $stage -Recurse -Depth 1 -Filter *.uplugin -File)
if ($descriptors.Count -ne 1) {
  Write-Failure "Expected one .uplugin in the zip's top-level folder, found $($descriptors.Count)."
  exit 1
}
$uplugin = $descriptors[0].FullName
$pluginDir = $descriptors[0].DirectoryName
$pluginName = $descriptors[0].BaseName
Write-Host "Extracted $Zip -> $pluginDir"

# --- Self-contained (WP-F1) ---
if ($RequireStandalone) {
  $needed = @(
    "Source\Open3DStreamCore\Open3DStreamCore.Build.cs",
    "Source\ThirdParty\Open3DStreamCore\SYNC_STAMP.txt",
    "Source\ThirdParty\Open3DStreamCore\o3ds_generated.h"
  )
  $absent = @($needed | Where-Object { !(Test-Path -LiteralPath (Join-Path $pluginDir $_)) })
  # Filter by name: Windows PowerShell 5.1 ignores -Include when combined with -LiteralPath.
  $stale = @(Get-ChildItem -LiteralPath $pluginDir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "open3dstreamstatic*" -or $_.Name -ieq "flatbuffers.lib" })
  if ($absent.Count -gt 0 -or $stale.Count -gt 0) {
    if ($absent.Count -gt 0) { Write-Failure "The Fab zip lacks $($absent -join ', '), so it cannot build on its own (WP-F1). Run Build/Scripts/sync_o3ds_core.py and commit the result." }
    if ($stale.Count -gt 0) { Write-Failure "The Fab zip contains prebuilt core libraries that WP-F1 replaced: $(($stale | ForEach-Object { $_.Name }) -join ', ')" }
    exit 1
  }
  Write-Host "[OK] The zip holds the Open3DStreamCore module and its core mirror; nothing is added before BuildPlugin."
}

# --- BuildPlugin, failing on warnings in plugin sources ---
& (Join-Path $PSScriptRoot "Build-Plugin.ps1") `
  -UEPath $UEPath `
  -PluginUPluginPath $uplugin `
  -OutDir $package `
  -TargetPlatforms @("Win64") `
  -Configuration "Development" `
  -FailOnWarnings
if ($LASTEXITCODE -ne 0) {
  Write-Failure "BuildPlugin failed on the Fab zip's contents (exit code $LASTEXITCODE)."
  exit 1
}

# --- Check the binaries BuildPlugin produced ---
$problems = @()
$modules = @((Get-Content -LiteralPath $uplugin -Raw | ConvertFrom-Json).Modules | ForEach-Object { $_.Name })
$binDir = Join-Path $package "Binaries\Win64"
foreach ($m in $modules) {
  $dll = Join-Path $binDir "UnrealEditor-$m.dll"
  if (!(Test-Path -LiteralPath $dll)) {
    $problems += "BuildPlugin produced no editor binary for module $m ($dll)."
  }
}
$excludeList = Join-Path $PSScriptRoot "..\Fab\exclude-modules.txt"
if (Test-Path -LiteralPath $excludeList) {
  $excluded = @(Get-Content -LiteralPath $excludeList | ForEach-Object { ($_ -split '#')[0].Trim() } | Where-Object { $_ })
  foreach ($m in $excluded) {
    $leaked = @(Get-ChildItem -LiteralPath $package -Recurse -File -Filter "*$m*" -ErrorAction SilentlyContinue)
    if ($leaked.Count -gt 0) {
      $problems += "Excluded module $m has files in the BuildPlugin output: $(($leaked | Select-Object -First 3 | ForEach-Object { $_.Name }) -join ', ')"
    }
  }
}
# WebRTC is the separate Open3DBroadcastWebRTC add-on (ADR 0002, WP-F11); nothing of it may be in
# the main plugin's package.
$livekit = @(Get-ChildItem -LiteralPath $package -Recurse -File -Filter "*livekit*" -ErrorAction SilentlyContinue)
$webrtc = @(Get-ChildItem -LiteralPath $package -Recurse -File -Filter "*Open3DTransportWebRTC*" -ErrorAction SilentlyContinue)
if ($livekit.Count -gt 0 -or $webrtc.Count -gt 0) {
  $problems += "WebRTC add-on files in the BuildPlugin output: $((@($livekit) + @($webrtc) | Select-Object -First 5 | ForEach-Object { $_.Name }) -join ', ')"
}

if ($problems.Count -gt 0) {
  foreach ($p in $problems) { Write-Failure $p }
  exit 1
}
Write-Host "[OK] The Fab zip builds with BuildPlugin and produced editor binaries for: $($modules -join ', ')"
exit 0
