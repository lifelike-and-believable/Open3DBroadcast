# WP-F2 acceptance: a game target that also targets Linux builds without error, with the
# Open3DBroadcast modules left out on Linux (ADR 0001).
#
# Builds the ProjectSandbox game target (which enables the plugin) for Linux with UBT through
# Engine\Build\BatchFiles\Build.bat, then checks that UBT produced no intermediate folder for any
# Open3D* module under the plugin.
#
# Needs, on the Windows machine that runs it:
#   - the Linux cross-compile toolchain, with LINUX_MULTIARCH_ROOT set (Epic's "Cross-Compiling
#     for Linux" page lists the toolchain version for each engine release);
#   - an engine that includes the Linux target platform (for a launcher install, the "Linux"
#     option under Target Platforms).
# Without LINUX_MULTIARCH_ROOT the script prints a notice and exits 0 unless -Require is given,
# so the nightly workflow can call it on a runner that has no toolchain yet.
param(
  [Parameter(Mandatory = $true)][string]$UEPath,
  [string]$ProjectFile = "",
  [string]$Configuration = "Development",
  # Fail instead of skipping when the Linux toolchain is not configured.
  [switch]$Require
)

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($ProjectFile)) {
  $ProjectFile = Join-Path $repoRoot "ProjectSandbox\ProjectSandbox.uproject"
}
$ProjectFile = (Resolve-Path -LiteralPath $ProjectFile).Path
$projectDir = Split-Path -Parent $ProjectFile
$targetName = [IO.Path]::GetFileNameWithoutExtension($ProjectFile)

if ([string]::IsNullOrWhiteSpace($env:LINUX_MULTIARCH_ROOT)) {
  $message = "LINUX_MULTIARCH_ROOT is not set, so the Linux cross-compile toolchain is not available. The Linux exclusion check did not run; see Build/README.md, 'Platforms'."
  if ($Require) {
    Write-Host "::error::$message"
    exit 1
  }
  Write-Host "::notice::$message"
  exit 0
}

$buildBat = Join-Path $UEPath "Engine\Build\BatchFiles\Build.bat"
if (!(Test-Path -LiteralPath $buildBat)) {
  Write-Host "::error::Build.bat not found at '$buildBat'."
  exit 1
}

$pluginDir = Join-Path $projectDir "Plugins\Open3DBroadcast"
$pluginIntermediate = Join-Path $pluginDir "Intermediate\Build"

# Remove Linux intermediates from an earlier run, so the check below only sees this build.
if (Test-Path -LiteralPath $pluginIntermediate) {
  Get-ChildItem -LiteralPath $pluginIntermediate -Directory -Filter "Linux*" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
}

Write-Host "Building $targetName Linux $Configuration (the plugin's modules must be left out)"
& $buildBat $targetName Linux $Configuration "-Project=$ProjectFile" -WaitMutex
if ($LASTEXITCODE -ne 0) {
  Write-Host "::error::UBT failed for $targetName Linux $Configuration with exit code $LASTEXITCODE. With the plugin enabled, a Linux game target must build (ADR 0001)."
  exit 1
}

$built = @()
if (Test-Path -LiteralPath $pluginIntermediate) {
  $built = @(Get-ChildItem -LiteralPath $pluginIntermediate -Directory -Filter "Linux*" -ErrorAction SilentlyContinue |
    ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Directory -Recurse -Filter "Open3D*" -ErrorAction SilentlyContinue })
}
if ($built.Count -gt 0) {
  foreach ($dir in $built) {
    Write-Host "::error::Plugin module compiled for Linux: $($dir.FullName)"
  }
  Write-Host "::error::The Linux build succeeded but compiled Open3DBroadcast modules; they should be left out by PlatformAllowList."
  exit 1
}

Write-Host "[OK] $targetName built for Linux with no Open3DBroadcast module compiled."
exit 0
