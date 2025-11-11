param(
  [string]$UEPath,
  [string]$ProjectFile,
  [string]$TestFilter,
  [string]$ResultsDir = "Artifacts\Tests",
  [string]$RunLabel
)

# Sanitize and normalize
$UEPath = $UEPath.Trim('"', "'")
$ProjectFile = $ProjectFile.Trim('"', "'")
$ResultsDir = $ResultsDir.Trim('"', "'")
$TestFilter = $TestFilter ? $TestFilter.Trim() : ""

if ([string]::IsNullOrWhiteSpace($TestFilter)) {
  Write-Error "TestFilter cannot be empty. Provide fully-qualified automation test names separated by '+' or a group specifier (for example 'Group:MyGroup')."
  exit 1
}

$EditorExe = Join-Path -Path $UEPath -ChildPath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (!(Test-Path -LiteralPath $EditorExe)) {
  Write-Error "UnrealEditor-Cmd not found under $UEPath"
  exit 1
}

if (!(Test-Path -LiteralPath $ProjectFile)) {
  Write-Error "Project file not found: $ProjectFile"
  exit 1
}

if ([string]::IsNullOrWhiteSpace($RunLabel)) {
  $RunLabel = $TestFilter -replace "[^a-zA-Z0-9_.-]", "_"
  if ([string]::IsNullOrWhiteSpace($RunLabel)) {
    $RunLabel = "Automation"
  }
}

$RunLabel = $RunLabel.Trim('"', "'")

$ReportPath = Join-Path -Path $ResultsDir -ChildPath $RunLabel
$createdDir = New-Item -ItemType Directory -Force -Path $ReportPath
$ReportPath = (Resolve-Path -LiteralPath $createdDir.FullName).Path
$LogPath = Join-Path -Path $ReportPath -ChildPath "Automation.log"
$ReportIndex = Join-Path -Path $ReportPath -ChildPath "index.json"

Write-Host "Running automation tests..."
Write-Host "  Editor: $EditorExe"
Write-Host "  Project: $ProjectFile"
Write-Host "  Filter: $TestFilter"
Write-Host "  Report:  $ReportPath"
Write-Host "  Log:     $LogPath"

# Keep the entire ExecCmds as a single argument token so semicolons/spaces are preserved
$cmds = "Automation RunTests $TestFilter; Quit"

& $EditorExe `
  "$ProjectFile" `
  -unattended -nop4 -NullRHI -NoSound -NoSplash `
  "-log=$LogPath" `
  "-ReportExportPath=$ReportPath" `
  "-ExecCmds=$cmds"

if ($LASTEXITCODE -eq 0) {
  Write-Host "[OK] Tests completed successfully"
  if (Test-Path $ReportIndex) {
    Write-Host "[OK] JSON report: $ReportIndex"
  } else {
    Write-Warning "Automation exited without producing index.json. Check $LogPath for details."
  }
} else {
  Write-Error "Tests failed with exit code $LASTEXITCODE"
}

exit $LASTEXITCODE
