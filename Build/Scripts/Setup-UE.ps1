param([string]$UEPath = "C:\Program Files\Epic Games\UE_5.4")

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

$resolvedUE = (Resolve-Path -LiteralPath $UEPath).Path
Write-Host "✓ UE setup OK: $resolvedUE"
Write-Host "✓ RunUAT found: $UAT"
