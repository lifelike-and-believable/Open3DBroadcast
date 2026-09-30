param(
  [string]$UEPath,
  # Run the tests in this project. Use either -ProjectFile or -PluginPackageDir.
  [string]$ProjectFile,
  # Run the tests against a BuildPlugin package instead. The script creates a
  # throwaway host project with the package as its only project plugin, so the tests
  # load exactly the binaries that BuildPlugin just produced and nothing is compiled.
  [string]$PluginPackageDir,
  # Where the throwaway host project is created (default: a temp folder). It is deleted
  # and recreated on every run.
  [string]$HostProjectDir,
  # Engine plugins enabled in the throwaway host project besides the tested plugin.
  [string[]]$HostProjectPlugins = @("LiveLink"),
  # Plain name prefix, as used by 'Automation RunTests'. Every plugin test starts with
  # "Open3DBroadcast." (ADR 0006).
  [string]$TestFilter = "Open3DBroadcast",
  [string]$ResultsDir = "Artifacts\Tests",
  [string]$RunLabel
)

# Exit codes: 0 every test ran and passed; 1 a test failed, no test ran, the report is
# missing or unreadable, or the editor exited with an error; 2 bad arguments.

function Write-Failure([string]$Message) {
  # ::error:: is a GitHub Actions annotation; elsewhere it is just a message.
  Write-Host "::error::$Message"
}

# Sanitize and normalize
if ($UEPath) { $UEPath = $UEPath.Trim('"', "'") }
if ($ProjectFile) { $ProjectFile = $ProjectFile.Trim('"', "'") }
if ($PluginPackageDir) { $PluginPackageDir = $PluginPackageDir.Trim('"', "'") }
$ResultsDir = $ResultsDir.Trim('"', "'")
$TestFilter = if ($TestFilter) { $TestFilter.Trim() } else { "" }

if ([string]::IsNullOrWhiteSpace($TestFilter)) {
  Write-Failure "TestFilter cannot be empty."
  exit 2
}
# 'Automation RunTests' matches by name substring; a trailing '.*' or '*' is not a
# wildcard there and can make the filter match nothing (CI-3).
if ($TestFilter -match '\.?\*$') {
  $trimmed = $TestFilter -replace '\.?\*$', ''
  Write-Warning "TestFilter '$TestFilter' ends in a wildcard, which RunTests does not use. Using '$trimmed'."
  $TestFilter = $trimmed
}

if ([string]::IsNullOrWhiteSpace($UEPath)) {
  Write-Failure "-UEPath is required."
  exit 2
}
$EditorExe = Join-Path -Path $UEPath -ChildPath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if (!(Test-Path -LiteralPath $EditorExe)) {
  Write-Failure "UnrealEditor-Cmd not found under $UEPath"
  exit 2
}

if ([string]::IsNullOrWhiteSpace($ProjectFile) -eq [string]::IsNullOrWhiteSpace($PluginPackageDir)) {
  Write-Failure "Pass exactly one of -ProjectFile or -PluginPackageDir."
  exit 2
}

if ($PluginPackageDir) {
  if (!(Test-Path -LiteralPath $PluginPackageDir)) {
    Write-Failure "Plugin package not found: $PluginPackageDir"
    exit 2
  }
  $descriptor = Get-ChildItem -LiteralPath $PluginPackageDir -Filter *.uplugin -File | Select-Object -First 1
  if (-not $descriptor) {
    Write-Failure "No .uplugin in $PluginPackageDir"
    exit 2
  }
  if (!(Test-Path -LiteralPath (Join-Path $PluginPackageDir "Binaries\Win64"))) {
    Write-Failure "$PluginPackageDir has no Binaries\Win64. Pass the output folder of a successful BuildPlugin run."
    exit 2
  }
  $pluginName = $descriptor.BaseName

  if ([string]::IsNullOrWhiteSpace($HostProjectDir)) {
    $HostProjectDir = Join-Path ([IO.Path]::GetTempPath()) "$pluginName-TestHost"
  }
  if (Test-Path -LiteralPath $HostProjectDir) {
    Remove-Item -LiteralPath $HostProjectDir -Recurse -Force
  }
  $hostPlugins = Join-Path $HostProjectDir "Plugins"
  New-Item -ItemType Directory -Force -Path $hostPlugins | Out-Null
  $hostPluginDir = Join-Path $hostPlugins $pluginName
  Copy-Item -LiteralPath $PluginPackageDir -Destination $hostPluginDir -Recurse
  # BuildPlugin normally deletes its HostProject folder from the package. If one is left
  # over it holds a second copy of the plugin, which the editor would also discover.
  $leftover = Join-Path $hostPluginDir "HostProject"
  if (Test-Path -LiteralPath $leftover) {
    Remove-Item -LiteralPath $leftover -Recurse -Force
  }

  $entries = @(@{ Name = $pluginName; Enabled = $true })
  foreach ($p in $HostProjectPlugins) { $entries += @{ Name = $p; Enabled = $true } }
  $ProjectFile = Join-Path $HostProjectDir "$($pluginName)TestHost.uproject"
  @{
    FileVersion = 3
    EngineAssociation = ""
    Category = ""
    Description = "Throwaway host project for $pluginName automation tests"
    Plugins = $entries
  } | ConvertTo-Json -Depth 4 | Out-File -FilePath $ProjectFile -Encoding ascii
  Write-Host "Created host project $ProjectFile with the package from $PluginPackageDir"
}

if (!(Test-Path -LiteralPath $ProjectFile)) {
  Write-Failure "Project file not found: $ProjectFile"
  exit 2
}

if ([string]::IsNullOrWhiteSpace($RunLabel)) {
  $RunLabel = $TestFilter -replace "[^a-zA-Z0-9_.-]", "_"
  if ([string]::IsNullOrWhiteSpace($RunLabel)) {
    $RunLabel = "Automation"
  }
}
$RunLabel = $RunLabel.Trim('"', "'")

$ReportPath = Join-Path -Path $ResultsDir -ChildPath $RunLabel
# Never read a report left over from an earlier run.
if (Test-Path -LiteralPath $ReportPath) {
  Remove-Item -LiteralPath $ReportPath -Recurse -Force
}
$createdDir = New-Item -ItemType Directory -Force -Path $ReportPath
$ReportPath = (Resolve-Path -LiteralPath $createdDir.FullName).Path
$LogPath = Join-Path -Path $ReportPath -ChildPath "Automation.log"
$ReportIndex = Join-Path -Path $ReportPath -ChildPath "index.json"

Write-Host "Running automation tests..."
Write-Host "  Editor:  $EditorExe"
Write-Host "  Project: $ProjectFile"
Write-Host "  Filter:  $TestFilter"
Write-Host "  O3DB_NETWORK_TESTS: $(if ($env:O3DB_NETWORK_TESTS) { $env:O3DB_NETWORK_TESTS } else { '(unset)' })"
Write-Host "  Report:  $ReportPath"
Write-Host "  Log:     $LogPath"

# Keep the entire ExecCmds as a single argument token so semicolons/spaces are preserved.
# 'Quit' inside the Automation command exits once the queued tests finish; -TestExit is a
# second way out if that does not happen.
$cmds = "Automation RunTests $TestFilter; Quit"

& $EditorExe `
  "$ProjectFile" `
  -unattended -nop4 -NullRHI -NoSound -NoSplash `
  "-log=$LogPath" `
  "-ReportExportPath=$ReportPath" `
  "-ExecCmds=$cmds" `
  "-TestExit=Automation Test Queue Empty"
$editorExit = $LASTEXITCODE
if ($null -eq $editorExit) { $editorExit = 1 }

# --- Evaluate the result. The editor's exit code alone is not trusted (CI-3). ---
$problems = @()
if ($editorExit -ne 0) {
  $problems += "UnrealEditor-Cmd exited with code $editorExit. See $LogPath."
}

$tests = @()
$report = $null
if (!(Test-Path -LiteralPath $ReportIndex)) {
  $problems += "No automation report was written ($ReportIndex is missing). The editor may have failed to start, crashed, or not run the tests. See $LogPath."
} else {
  try {
    $report = Get-Content -LiteralPath $ReportIndex -Raw -Encoding UTF8 | ConvertFrom-Json
    $tests = @($report.tests)
  } catch {
    $problems += "The automation report $ReportIndex could not be parsed: $($_.Exception.Message)"
  }
}

if ($report) {
  $passed = @($tests | Where-Object { $_.state -eq 'Success' })
  $skipped = @($tests | Where-Object { $_.state -eq 'Skipped' })
  # Fail, NotRun (requested but never executed) and InProcess (still running when the
  # editor exited, usually a crash or hang) all count as failures.
  $bad = @($tests | Where-Object { $_.state -ne 'Success' -and $_.state -ne 'Skipped' })

  Write-Host ""
  Write-Host "Automation report: $($tests.Count) test(s), $($passed.Count) passed, $($bad.Count) failed or not finished, $($skipped.Count) skipped"

  if (($tests.Count - $skipped.Count) -eq 0) {
    $problems += "No test ran for filter '$TestFilter'. A filter that matches nothing is a failure, not a pass."
  }
  if ($report.failed -and [int]$report.failed -gt 0 -and $bad.Count -eq 0) {
    $problems += "The report header counts $($report.failed) failed test(s)."
  }
  foreach ($t in $bad) {
    $name = if ($t.fullTestPath) { $t.fullTestPath } else { $t.testDisplayName }
    $firstError = $null
    foreach ($e in @($t.entries)) {
      if ($e.event -and $e.event.type -eq 'Error') {
        $firstError = $e.event.message
        break
      }
    }
    $detail = if ($firstError) { ": $firstError" } else { "" }
    $problems += "$name [$($t.state)]$detail"
  }
  foreach ($t in $skipped) {
    Write-Host "  skipped: $($t.fullTestPath)"
  }
}

if ($env:GITHUB_STEP_SUMMARY) {
  $lines = @("### Automation tests ($TestFilter)", "")
  if ($report) {
    $lines += "$($tests.Count) test(s): $(@($tests | Where-Object { $_.state -eq 'Success' }).Count) passed."
  }
  if ($problems.Count -eq 0) {
    $lines += "", "**Passed.**"
  } else {
    $lines += "", "**Failed:**", ""
    foreach ($p in $problems) { $lines += "- $p" }
  }
  $lines | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Append -Encoding utf8
}

if ($problems.Count -gt 0) {
  foreach ($p in $problems) { Write-Failure $p }
  # The log is only in the uploaded artifact otherwise; print its end so a startup
  # failure or crash can be diagnosed from the job log alone.
  if (Test-Path -LiteralPath $LogPath) {
    Write-Host "----- Last 150 lines of $LogPath -----"
    Get-Content -LiteralPath $LogPath -Tail 150 | ForEach-Object { Write-Host $_ }
    Write-Host "----- End of $LogPath -----"
  }
  exit 1
}

Write-Host "[OK] All $($tests.Count) automation test(s) passed. Report: $ReportIndex"
exit 0
