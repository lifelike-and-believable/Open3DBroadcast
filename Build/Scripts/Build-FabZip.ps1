param(
  [string]$UEPath,
  # The Fab source zip made by Build/Scripts/fab-package.py, or a folder that contains it.
  [string]$Zip,
  # Scratch folder: the zip is extracted to <WorkDir>\Stage and packaged to <WorkDir>\Package.
  [string]$WorkDir,
  # Plugin folder of the checkout whose ThirdParty\open3dstream was filled by
  # Sync-O3DSCore.ps1. Used only while the zip cannot build on its own (see below).
  [string]$CoreSourcePluginDir,
  # Fail instead of copying the core library in. Set this once WP-F1 lands.
  [switch]$RequireStandalone
)

# Runs RunUAT BuildPlugin on the contents of the Fab source zip, the way Fab's build
# farm would, then checks the resulting binaries (CI-5).
#
# WP-F1 gap: the zip holds only git-tracked files, and the o3ds core library and headers
# (ThirdParty\open3dstream\lib and \include) are build outputs of Sync-O3DSCore.ps1, so
# the zip alone does not build yet. Until WP-F1 compiles the core as a module, this
# script copies the core built for the same commit into the extracted tree and emits a
# warning annotation saying so. -RequireStandalone turns that into a failure.
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

# --- WP-F1 gap (see the header comment) ---
$coreParts = @("ThirdParty\open3dstream\lib", "ThirdParty\open3dstream\include")
$missing = @($coreParts | Where-Object { !(Test-Path -LiteralPath (Join-Path $pluginDir $_)) })
if ($missing.Count -gt 0) {
  if ($RequireStandalone) {
    Write-Failure "The Fab zip does not contain $($missing -join ', '), so it cannot build on its own (WP-F1)."
    exit 1
  }
  if ([string]::IsNullOrWhiteSpace($CoreSourcePluginDir)) {
    Write-Failure "The Fab zip lacks $($missing -join ', ') and no -CoreSourcePluginDir was given to copy them from."
    exit 2
  }
  foreach ($part in $missing) {
    $src = Join-Path $CoreSourcePluginDir $part
    if (!(Test-Path -LiteralPath $src)) {
      Write-Failure "$src not found. Run Build/Scripts/Sync-O3DSCore.ps1 first."
      exit 2
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $pluginDir $part) -Recurse
  }
  Write-Host "::warning title=Fab zip does not build on its own (WP-F1)::The zip has no $($missing -join ' or '). This check copied the core built by Sync-O3DSCore.ps1 for this commit into the extracted tree before BuildPlugin. Fab's build farm cannot do that, so the zip is not yet submittable. WP-F1 removes this step."
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
$livekit = @(Get-ChildItem -LiteralPath $package -Recurse -File -Filter "*livekit*" -ErrorAction SilentlyContinue)
if ($livekit.Count -gt 0) {
  $problems += "livekit files in the BuildPlugin output: $(($livekit | ForEach-Object { $_.Name }) -join ', ')"
}

if ($problems.Count -gt 0) {
  foreach ($p in $problems) { Write-Failure $p }
  exit 1
}
Write-Host "[OK] The Fab zip builds with BuildPlugin and produced editor binaries for: $($modules -join ', ')"
exit 0
