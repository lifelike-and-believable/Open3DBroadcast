# Builds the Open3DBroadcastWebRTC add-on plugin against a built Open3DBroadcast package (WP-F11,
# ADR 0002) and stages the add-on as a distributable plugin folder.
#
# RunUAT BuildPlugin cannot build the add-on on its own: it builds a plugin in a generated host
# project that contains only that plugin, so the add-on's dependency on Open3DBroadcast cannot be
# resolved there. Instead this script makes a throwaway host project the way a user's project
# looks when both plugins are installed:
#
#   <HostProjectDir>/
#     <HostName>.uproject                  enables Open3DBroadcast, Open3DBroadcastWebRTC, LiveLink
#     Plugins/Open3DBroadcast/             copy of -HostPluginPackageDir (a BuildPlugin package)
#     Plugins/Open3DBroadcastWebRTC/       copy of the add-on source (-AddOnDir)
#
# and runs UnrealBuildTool for the editor target of that project. The BuildPlugin package carries
# Source/ and Binaries/ but no Intermediate/, so UBT may recompile Open3DBroadcast's modules inside
# the host copy; the add-on compiles against the package's public headers either way.
#
# Afterwards the host project can run the automation tests with both plugins enabled
# (Run-AutomationTests.ps1 -ProjectFile <HostProjectDir>\<HostName>.uproject), and -OutDir holds
# the add-on plugin folder (Source, Binaries\Win64, Config, Resources, docs; no Intermediate).
#
# Exit codes: 0 built and staged; 1 the build failed or (with -FailOnWarnings) the compiler
# reported a warning in an add-on source file; 2 bad arguments.
param(
  [Parameter(Mandatory = $true)][string]$UEPath,
  # Output folder of a successful Build-Plugin.ps1 / RunUAT BuildPlugin run for Open3DBroadcast.
  [Parameter(Mandatory = $true)][string]$HostPluginPackageDir,
  # The add-on plugin source. Default: ProjectSandbox\Plugins\Open3DBroadcastWebRTC in this checkout.
  [string]$AddOnDir,
  # Deleted and recreated on every run.
  [Parameter(Mandatory = $true)][string]$HostProjectDir,
  # Receives the staged add-on plugin folder. Deleted and recreated on every run.
  [Parameter(Mandatory = $true)][string]$OutDir,
  [string]$HostName = "O3DWebRTCHost",
  [string]$Configuration = "Development",
  # No precompiled headers and no unity build, like BuildPlugin -StrictIncludes (CI-4): every
  # source file must include what it uses.
  [switch]$StrictIncludes,
  # Fail when the compiler reports a warning in a file under the add-on (CI-4).
  [switch]$FailOnWarnings
)

function Write-Failure([string]$Message) {
  # ::error:: is a GitHub Actions annotation; elsewhere it is just a message.
  Write-Host "::error::$Message"
}

$AddOnName = "Open3DBroadcastWebRTC"
$HostPluginName = "Open3DBroadcast"

$UEPath = $UEPath.Trim('"', "'")
$HostPluginPackageDir = $HostPluginPackageDir.Trim('"', "'")
$HostProjectDir = $HostProjectDir.Trim('"', "'")
$OutDir = $OutDir.Trim('"', "'")
if ([string]::IsNullOrWhiteSpace($AddOnDir)) {
  $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
  $AddOnDir = Join-Path $repoRoot "ProjectSandbox\Plugins\$AddOnName"
}
$AddOnDir = $AddOnDir.Trim('"', "'")

$buildBat = Join-Path $UEPath "Engine\Build\BatchFiles\Build.bat"
if (!(Test-Path -LiteralPath $buildBat)) {
  Write-Failure "Build.bat not found under $UEPath"
  exit 2
}
if (!(Test-Path -LiteralPath (Join-Path $HostPluginPackageDir "$HostPluginName.uplugin"))) {
  Write-Failure "$HostPluginPackageDir has no $HostPluginName.uplugin. Pass the output folder of a successful BuildPlugin run."
  exit 2
}
if (!(Test-Path -LiteralPath (Join-Path $HostPluginPackageDir "Binaries\Win64"))) {
  Write-Failure "$HostPluginPackageDir has no Binaries\Win64. Pass the output folder of a successful BuildPlugin run."
  exit 2
}
if (!(Test-Path -LiteralPath (Join-Path $AddOnDir "$AddOnName.uplugin"))) {
  Write-Failure "$AddOnDir has no $AddOnName.uplugin."
  exit 2
}

# --- Host project ---
if (Test-Path -LiteralPath $HostProjectDir) {
  Remove-Item -LiteralPath $HostProjectDir -Recurse -Force
}
$hostPlugins = Join-Path $HostProjectDir "Plugins"
New-Item -ItemType Directory -Force -Path $hostPlugins | Out-Null
$HostProjectDir = (Resolve-Path -LiteralPath $HostProjectDir).Path
$hostPlugins = Join-Path $HostProjectDir "Plugins"

$hostCopy = Join-Path $hostPlugins $HostPluginName
Copy-Item -LiteralPath $HostPluginPackageDir -Destination $hostCopy -Recurse
# BuildPlugin normally deletes its HostProject folder from the package. A leftover one holds a
# second copy of the plugin, which UBT and the editor would also discover.
$leftover = Join-Path $hostCopy "HostProject"
if (Test-Path -LiteralPath $leftover) {
  Remove-Item -LiteralPath $leftover -Recurse -Force
}

$addOnCopy = Join-Path $hostPlugins $AddOnName
Copy-Item -LiteralPath $AddOnDir -Destination $addOnCopy -Recurse
# A local checkout may hold build output from an earlier ProjectSandbox build.
foreach ($stale in @("Binaries", "Intermediate")) {
  $p = Join-Path $addOnCopy $stale
  if (Test-Path -LiteralPath $p) {
    Remove-Item -LiteralPath $p -Recurse -Force
  }
}

$ProjectFile = Join-Path $HostProjectDir "$HostName.uproject"
@{
  FileVersion = 3
  EngineAssociation = ""
  Category = ""
  Description = "Throwaway host project: $AddOnName built against a $HostPluginName package"
  Plugins = @(
    @{ Name = $HostPluginName; Enabled = $true },
    @{ Name = $AddOnName; Enabled = $true },
    @{ Name = "LiveLink"; Enabled = $true }
  )
} | ConvertTo-Json -Depth 4 | Out-File -FilePath $ProjectFile -Encoding ascii
Write-Host "Created host project $ProjectFile"
Write-Host "  $HostPluginName from $HostPluginPackageDir"
Write-Host "  $AddOnName from $AddOnDir"

# --- Build ---
$logPath = Join-Path (Split-Path -Parent $HostProjectDir) ((Split-Path -Leaf $HostProjectDir) + "-UBT.log")
Write-Host "Building UnrealEditor Win64 $Configuration for the host project (log: $logPath)"
# The host project has no Source folder, so its editor target is the engine's UnrealEditor target;
# UBT then builds only the project's plugins (the engine is precompiled).
$ubtArgs = @("UnrealEditor", "Win64", $Configuration, "-Project=$ProjectFile", "-WaitMutex", "-NoHotReload")
if ($StrictIncludes) {
  $ubtArgs += @("-NoPCH", "-NoSharedPCH", "-DisableUnity")
}
& $buildBat $ubtArgs | Tee-Object -FilePath $logPath
$buildExit = $LASTEXITCODE
if ($null -eq $buildExit) { $buildExit = 1 }
if ($buildExit -ne 0) {
  Write-Failure "Building $AddOnName against the $HostPluginName package failed with exit code $buildExit. See the compiler errors above."
  exit 1
}

$addOnDll = Join-Path $addOnCopy "Binaries\Win64\UnrealEditor-Open3DTransportWebRTC.dll"
if (!(Test-Path -LiteralPath $addOnDll)) {
  Write-Failure "The build succeeded but produced no $addOnDll."
  exit 1
}

if ($FailOnWarnings) {
  # MSVC: C:\...\Plugins\Open3DBroadcastWebRTC\Source\...\File.cpp(53,46): warning C4996: message
  $pattern = '^\s*(?<file>\S.*?[\\/]Plugins[\\/]' + [regex]::Escape($AddOnName) + '[\\/](?<rest>.+?))\((?<line>\d+)(,\d+)?\)\s*:\s*warning\s+(?<code>[A-Z]+\d+)\s*:\s*(?<msg>.*)$'
  $seen = @{}
  $warnings = @()
  foreach ($line in Get-Content -LiteralPath $logPath) {
    $m = [regex]::Match($line, $pattern)
    if (-not $m.Success) { continue }
    $key = "$($m.Groups['rest'].Value)|$($m.Groups['line'].Value)|$($m.Groups['code'].Value)"
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    $warnings += "$($m.Groups['rest'].Value -replace '\\', '/')($($m.Groups['line'].Value)): $($m.Groups['code'].Value) $($m.Groups['msg'].Value)"
  }
  if ($warnings.Count -gt 0) {
    foreach ($w in $warnings) { Write-Failure $w }
    Write-Failure "The compiler reported $($warnings.Count) warning(s) in $AddOnName sources. -FailOnWarnings treats them as errors."
    exit 1
  }
  Write-Host "[OK] No compiler warnings in $AddOnName sources"
}

# --- Stage the add-on plugin folder ---
if (Test-Path -LiteralPath $OutDir) {
  Remove-Item -LiteralPath $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
foreach ($item in Get-ChildItem -LiteralPath $addOnCopy) {
  if ($item.Name -eq "Intermediate") { continue }
  Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $OutDir $item.Name) -Recurse
}
# Debug symbols are release assets, not part of the plugin (FAB-4).
Get-ChildItem -LiteralPath $OutDir -Recurse -File -Filter "*.pdb" | Remove-Item -Force

Write-Host "[OK] $AddOnName built against $HostPluginName and staged at $OutDir"
Write-Host "     Host project for tests: $ProjectFile"
exit 0
