param(
  [string]$UEPath,
  [string]$ProjectFile,
  [string]$TestFilter = "*",
  [string]$ResultsDir = "Artifacts\Tests"
)

# Sanitize and normalize
$UEPath = $UEPath.Trim('"', "'")
$ProjectFile = $ProjectFile.Trim('"', "'")
$ResultsDir = $ResultsDir.Trim('"', "'")

$EditorExe = Join-Path -Path $UEPath -ChildPath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (!(Test-Path -LiteralPath $EditorExe)) {
  Write-Error "UnrealEditor-Cmd not found under $UEPath"
  exit 1
}

if (!(Test-Path -LiteralPath $ProjectFile)) {
  Write-Error "Project file not found: $ProjectFile"
  exit 1
}

New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null
$ResultsXml = Join-Path -Path $ResultsDir -ChildPath "Results.xml"

Write-Host "Running automation tests..."
Write-Host "  Editor: $EditorExe"
Write-Host "  Project: $ProjectFile"
Write-Host "  Filter: $TestFilter"
Write-Host "  Results: $ResultsXml"

# Keep the entire ExecCmds as a single argument token so semicolons/spaces are preserved
$cmds = "Automation RunTests $TestFilter; Automation WriteResults $ResultsXml; Quit"

& $EditorExe `
  "$ProjectFile" `
  -unattended -nop4 -NullRHI -NoSound -NoSplash -Nolog `
  "-ExecCmds=$cmds"

if ($LASTEXITCODE -eq 0) {
  Write-Host "✓ Tests completed successfully"
  if (Test-Path $ResultsXml) {
    Write-Host "✓ Results written to: $ResultsXml"
  }
} else {
  Write-Error "Tests failed with exit code $LASTEXITCODE"
}

exit $LASTEXITCODE
