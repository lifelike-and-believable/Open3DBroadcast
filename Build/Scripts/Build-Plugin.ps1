param(
  [string]$UEPath,
  [string]$PluginUPluginPath,
  [string]$OutDir,
  [string[]]$TargetPlatforms = @("Win64"),
  [string]$Configuration = "Development",
  [string[]]$AdditionalPluginDirectories = @()
)

function Get-DotNetHost([string]$EngineRoot) {
  $dotNetRoot = Join-Path $EngineRoot "Engine\Binaries\ThirdParty\DotNet"
  if (!(Test-Path -LiteralPath $dotNetRoot)) {
    throw "Unable to locate ThirdParty dotnet host under '$dotNetRoot'."
  }

  $dotnet = Get-ChildItem -LiteralPath $dotNetRoot -Recurse -Filter dotnet.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $dotnet) {
    throw "Could not find dotnet.exe under '$dotNetRoot'."
  }
  return $dotnet.FullName
}

function Invoke-UBTBuild {
  param(
    [string]$EngineRoot,
    [string]$Target,
    [string]$Platform,
    [string]$Configuration,
    [string]$ProjectPath
  )

  $dotnetExe = Get-DotNetHost -EngineRoot $EngineRoot
  $ubtDll = Join-Path $EngineRoot "Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll"
  if (!(Test-Path -LiteralPath $ubtDll)) {
    throw "UnrealBuildTool.dll not found at '$ubtDll'."
  }

  $logDir = Join-Path (Split-Path -Parent $ProjectPath) "Saved\Logs"
  New-Item -ItemType Directory -Force -Path $logDir | Out-Null
  $logFile = Join-Path $logDir "$Target-$Platform-$Configuration.ubt.log"

  $args = @(
    $Target,
    $Platform,
    $Configuration,
    "-Project=$ProjectPath",
    "-WaitMutex",
    "-NoHotReload",
    "-log=$logFile"
  )

  Write-Host "Invoking UBT: $Target $Platform $Configuration"
  & $dotnetExe $ubtDll $args
  if ($LASTEXITCODE -ne 0) {
    throw "UBT failed for target '$Target' ($Platform/$Configuration). See $logFile for details."
  }
}

function Invoke-ProjectSandboxPackaging {
  param(
    [string]$EngineRoot,
    [string]$PluginDescriptor,
    [string]$OutDir,
    [string[]]$TargetPlatforms,
    [string]$Configuration,
    [string[]]$AdditionalPluginDirectories
  )

  $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
  $sandboxUProject = Join-Path $repoRoot "ProjectSandbox\ProjectSandbox.uproject"
  if (!(Test-Path -LiteralPath $sandboxUProject)) {
    throw "ProjectSandbox.uproject not found at '$sandboxUProject'. Cannot use fallback build path."
  }

  foreach ($platform in $TargetPlatforms) {
    Invoke-UBTBuild -EngineRoot $EngineRoot -Target "ProjectSandboxEditor" -Platform $platform -Configuration $Configuration -ProjectPath $sandboxUProject
    Invoke-UBTBuild -EngineRoot $EngineRoot -Target "ProjectSandbox" -Platform $platform -Configuration $Configuration -ProjectPath $sandboxUProject
  }

  if (Test-Path -LiteralPath $OutDir) {
    Remove-Item -LiteralPath $OutDir -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

  $hostProjectDir = Join-Path $OutDir "HostProject"
  $hostPluginsDir = Join-Path $hostProjectDir "Plugins"
  New-Item -ItemType Directory -Force -Path $hostPluginsDir | Out-Null

  $pluginRoot = Split-Path -Parent $PluginDescriptor
  $pluginName = [IO.Path]::GetFileNameWithoutExtension($PluginDescriptor)
  $destPluginDir = Join-Path $hostPluginsDir $pluginName
  Write-Host "Packaging plugin contents into $destPluginDir"
  Copy-Item -LiteralPath $pluginRoot -Destination $destPluginDir -Recurse -Force

  $additionalPlugins = @()
  foreach ($dir in $AdditionalPluginDirectories) {
    try {
      $resolvedDir = (Resolve-Path -LiteralPath $dir).Path
    } catch {
      Write-Warning "Additional plugin directory '$dir' cannot be resolved. Skipping."
      continue
    }
    $descriptor = Get-ChildItem -LiteralPath $resolvedDir -Filter *.uplugin -File | Select-Object -First 1
    if (-not $descriptor) {
      Write-Warning "No .uplugin descriptor found under '$resolvedDir'. Skipping."
      continue
    }
    $name = [IO.Path]::GetFileNameWithoutExtension($descriptor.FullName)
    $additionalPlugins += @{ Name = $name; Path = $descriptor.Directory.FullName }
  }

  foreach ($plugin in $additionalPlugins) {
    $dst = Join-Path $hostPluginsDir $plugin.Name
    Write-Host "Packaging additional plugin '$($plugin.Name)'"
    Copy-Item -LiteralPath $plugin.Path -Destination $dst -Recurse -Force
  }

  $pluginEntries = @(@{ Name = $pluginName; Enabled = $true })
  foreach ($plugin in $additionalPlugins) {
    $pluginEntries += @{ Name = $plugin.Name; Enabled = $true }
  }

  $hostUproject = @{
    FileVersion = 3
    EngineAssociation = ""
    Category = ""
    Description = "Auto-generated host project for packaging $pluginName"
    Plugins = $pluginEntries
  } | ConvertTo-Json -Depth 4

  $hostProjectFile = Join-Path $hostProjectDir "HostProject.uproject"
  $hostUproject | Out-File -FilePath $hostProjectFile -Encoding utf8 -NoNewline

  Write-Host "[OK] Fallback packaging completed at $OutDir"
  return 0
}

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
  exit 0
}

Write-Warning "RunUAT BuildPlugin failed with exit code $LASTEXITCODE. Attempting project-driven fallback build..."

try {
  Invoke-ProjectSandboxPackaging -EngineRoot $UEPath -PluginDescriptor $PluginUPluginPath -OutDir $OutDir -TargetPlatforms $TargetPlatforms -Configuration $Configuration -AdditionalPluginDirectories $AdditionalPluginDirectories
  exit 0
} catch {
  Write-Error $_.Exception.Message
  exit 1
}
