param(
  [string]$UEPath,
  [string]$ProjectFile,
  [string[]]$GauntletConfigs,
  [string]$OutputDir="Artifacts\Gauntlet",
  [switch]$NullRHI
)

$UAT = Join-Path $UEPath "Engine\Build\BatchFiles\RunUAT.bat"
if (!(Test-Path -LiteralPath $UAT)) {
  Write-Error "RunUAT not found under $UEPath"
  exit 1
}

if (!(Test-Path -LiteralPath $ProjectFile)) {
  Write-Error "Project file not found: $ProjectFile"
  exit 1
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$cfgs = $GauntletConfigs -join ","
$extra = $NullRHI.IsPresent ? "-NullRHI" : ""

Write-Host "Running Gauntlet tests..."
Write-Host "  UAT: $UAT"
Write-Host "  Project: $ProjectFile"
Write-Host "  Configs: $cfgs"
Write-Host "  Output: $OutputDir"
Write-Host "  NullRHI: $($NullRHI.IsPresent)"

& $UAT RunGauntlet `
  -Project="$ProjectFile" `
  -Config="$cfgs" `
  -ReportDir="$OutputDir" `
  $extra

if ($LASTEXITCODE -eq 0) {
  Write-Host "[OK] Gauntlet tests completed successfully"
} else {
  Write-Error "Gauntlet tests failed with exit code $LASTEXITCODE"
}

exit $LASTEXITCODE
