# WP-F7 acceptance: a packaged Win64 Shipping game with the plugin enabled builds, and none of the
# plugin's Editor-type modules is compiled into it (ADR 0010).
#
# 1. Builds the ProjectSandbox editor target (Development) with UBT, which the cook needs.
# 2. Runs RunUAT BuildCookRun for the ProjectSandbox game target: Win64, Shipping, build, cook,
#    stage, pak, archive. UBT refuses to build UnrealEd (and other editor-only engine modules) for
#    a game target, so a runtime module that still depends on one fails this step.
# 3. Checks the result: the archive holds the game executable, and UBT wrote no intermediate
#    folder for Open3DBroadcastEditor or Open3DBroadcastTests under the Shipping game target.
#
# Slow (a full cook), so it runs in the nightly workflow, not on pull requests. See
# Build/README.md, "Shipping game build".
param(
  [Parameter(Mandatory = $true)][string]$UEPath,
  [string]$ProjectFile = "",
  # Archive directory for the packaged game. Default: <repo>\Artifacts\ShippingGame.
  [string]$ArchiveDir = ""
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($ProjectFile)) {
  $ProjectFile = Join-Path $repoRoot "ProjectSandbox\ProjectSandbox.uproject"
}
$ProjectFile = (Resolve-Path -LiteralPath $ProjectFile).Path
$projectDir = Split-Path -Parent $ProjectFile
$targetName = [IO.Path]::GetFileNameWithoutExtension($ProjectFile)
if ([string]::IsNullOrWhiteSpace($ArchiveDir)) {
  $ArchiveDir = Join-Path $repoRoot "Artifacts\ShippingGame"
}

$buildBat = Join-Path $UEPath "Engine\Build\BatchFiles\Build.bat"
$runUat = Join-Path $UEPath "Engine\Build\BatchFiles\RunUAT.bat"
foreach ($tool in @($buildBat, $runUat)) {
  if (!(Test-Path -LiteralPath $tool)) {
    Write-Host "::error::Not found: '$tool'."
    exit 1
  }
}

$pluginDir = Join-Path $projectDir "Plugins\Open3DBroadcast"
$pluginIntermediate = Join-Path $pluginDir "Intermediate\Build\Win64"

# Remove Shipping game intermediates from an earlier run, so the check below only sees this build.
if (Test-Path -LiteralPath $pluginIntermediate) {
  Get-ChildItem -LiteralPath $pluginIntermediate -Directory -Recurse -Filter "Shipping" -ErrorAction SilentlyContinue |
    Where-Object { $_.Parent.Name -eq $targetName } |
    Remove-Item -Recurse -Force
}

Write-Host "Building ${targetName}Editor Win64 Development (needed to cook)"
& $buildBat "${targetName}Editor" Win64 Development "-Project=$ProjectFile" -WaitMutex
if ($LASTEXITCODE -ne 0) {
  Write-Host "::error::UBT failed for ${targetName}Editor Win64 Development with exit code $LASTEXITCODE."
  exit 1
}

Write-Host "Packaging $targetName Win64 Shipping into $ArchiveDir"
& $runUat BuildCookRun `
  "-project=$ProjectFile" `
  -noP4 -unattended -utf8output `
  -platform=Win64 `
  -clientconfig=Shipping `
  -build -nocompileeditor `
  -cook -stage -pak -archive `
  "-archivedirectory=$ArchiveDir"
if ($LASTEXITCODE -ne 0) {
  Write-Host "::error::BuildCookRun failed for $targetName Win64 Shipping with exit code $LASTEXITCODE. A runtime module that depends on an editor-only module fails here (ADR 0010)."
  exit 1
}

$exe = @(Get-ChildItem -LiteralPath $ArchiveDir -Recurse -Filter "$targetName*.exe" -ErrorAction SilentlyContinue)
if ($exe.Count -eq 0) {
  Write-Host "::error::BuildCookRun succeeded but no $targetName executable was archived under '$ArchiveDir'."
  exit 1
}

$editorModules = @()
if (Test-Path -LiteralPath $pluginIntermediate) {
  $editorModules = @(Get-ChildItem -LiteralPath $pluginIntermediate -Directory -Recurse -ErrorAction SilentlyContinue |
    Where-Object { ($_.Name -eq "Open3DBroadcastEditor" -or $_.Name -eq "Open3DBroadcastTests") -and $_.FullName -match "\\$targetName\\Shipping\\" })
}
if ($editorModules.Count -gt 0) {
  foreach ($dir in $editorModules) {
    Write-Host "::error::Editor module compiled into the Shipping game: $($dir.FullName)"
  }
  exit 1
}

Write-Host "[OK] $targetName packaged for Win64 Shipping ($($exe[0].FullName)); no Open3DBroadcast editor module was compiled into it."
exit 0
