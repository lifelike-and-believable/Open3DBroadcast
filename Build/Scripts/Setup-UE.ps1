param(
  [string]$UEPath = "C:\Program Files\Epic Games\UE_5.7",
  # X.Y the engine at UEPath must be (ADR 0014); checked against Engine\Build\Build.version. Empty: no check.
  [string]$EngineVersion
)

# Trim accidental quotes that may be passed from CI or shell wrappers
$UEPath = $UEPath.Trim('"', "'")

if (!(Test-Path -LiteralPath $UEPath)) {
  Write-Error "UE path not found: $UEPath"
  exit 1
}

$UAT = Join-Path -Path $UEPath -ChildPath "Engine\Build\BatchFiles\RunUAT.bat"
if (!(Test-Path -LiteralPath $UAT)) {
  Write-Error "RunUAT not found under $UEPath"
  exit 1
}

if ($EngineVersion) {
  $buildVersion = Join-Path -Path $UEPath -ChildPath "Engine\Build\Build.version"
  if (!(Test-Path -LiteralPath $buildVersion)) {
    Write-Error "Build.version not found under $UEPath; cannot confirm it is UE $EngineVersion"
    exit 1
  }
  $engine = Get-Content -LiteralPath $buildVersion -Raw | ConvertFrom-Json
  $actual = "$($engine.MajorVersion).$($engine.MinorVersion)"
  if ($actual -ne $EngineVersion) {
    Write-Error "$UEPath is UE $actual.$($engine.PatchVersion), not UE $EngineVersion"
    exit 1
  }
  Write-Host "[OK] Engine version: $actual.$($engine.PatchVersion)"
}

$resolvedUE = (Resolve-Path -LiteralPath $UEPath).Path
Write-Host "[OK] UE setup OK: $resolvedUE"
Write-Host "[OK] RunUAT found: $UAT"
exit 0
