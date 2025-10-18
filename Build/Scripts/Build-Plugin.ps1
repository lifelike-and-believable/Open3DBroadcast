param(
  [string]$UEPath,
  [string]$PluginUPluginPath,
  [string]$OutDir,
  [string[]]$TargetPlatforms = @("Win64"),
  [string]$Configuration = "Development",
  [string[]]$AdditionalPluginDirectories = @()
)

# Sanitize and normalize inputs
$UEPath = $UEPath.Trim('"', "'")
$PluginUPluginPath = $PluginUPluginPath.Trim('"', "'")
$OutDir = $OutDir.Trim('"', "'")
$TargetPlatforms = $TargetPlatforms | ForEach-Object { $_.Trim('"', "'") }
$platforms = ($TargetPlatforms -join ",")

$UAT = Join-Path -Path $UEPath -ChildPath "Engine\Build\BatchFiles\RunUAT.bat"
if (!(Test-Path -LiteralPath $UAT)) {
  Write-Error "RunUAT not found under $UEPath"
  exit 1
}

$PluginUPluginPath = (Resolve-Path -LiteralPath $PluginUPluginPath).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

Write-Host "Building plugin..."
Write-Host "  Plugin: $PluginUPluginPath"
Write-Host "  Output: $OutDir"
Write-Host "  Platforms: $platforms"
Write-Host "  Configuration: $Configuration"
if ($AdditionalPluginDirectories -and $AdditionalPluginDirectories.Count -gt 0) {
  Write-Host "  Additional Plugin Directories: $($AdditionalPluginDirectories -join '; ')"
}

# Build UAT arguments
$uatArgs = @(
  "BuildPlugin",
  "-Plugin=$PluginUPluginPath",
  "-Package=$OutDir",
  "-TargetPlatforms=$platforms",
  "-Configuration=$Configuration",
  "-Rocket",
  "-VeryVerbose",
  "-VS2022"
)

# Add additional plugin directories if specified
if ($AdditionalPluginDirectories -and $AdditionalPluginDirectories.Count -gt 0) {
  $pluginDirs = $AdditionalPluginDirectories -join ";"
  $uatArgs += "-AdditionalPluginDirectories=$pluginDirs"
}

# Pass each UAT option as a single token so PowerShell doesn't split values with spaces
& $UAT $uatArgs

if ($LASTEXITCODE -eq 0) {
  Write-Host "[OK] Plugin build completed successfully"
} else {
  Write-Error "Plugin build failed with exit code $LASTEXITCODE"
}

exit $LASTEXITCODE
