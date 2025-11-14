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
    "-LiveCoding=0",
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

  try {
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

  $resolvedAdditionalDirs = @()
  foreach ($dir in $AdditionalPluginDirectories) {
    try {
      $resolvedDir = (Resolve-Path -LiteralPath $dir).Path
    } catch {
      Write-Warning "Additional plugin directory '$dir' cannot be resolved. Skipping."
      continue
    }
    $resolvedAdditionalDirs += $resolvedDir
  }

  # Helper lookups for locating plugin folders when satisfying dependencies
  $searchRoots = @()
  $repoPluginRoots = @(
    (Join-Path $repoRoot "plugins"),
    (Join-Path $repoRoot "ProjectSandbox\Plugins")
  )
  foreach ($root in $repoPluginRoots) {
    if (Test-Path -LiteralPath $root) { $searchRoots += $root }
  }
  $searchRoots += $resolvedAdditionalDirs
  $enginePluginRoots = @(
    (Join-Path $EngineRoot "Engine\Plugins"),
    (Join-Path $EngineRoot "Engine\Restricted")
  )
  foreach ($root in $enginePluginRoots) {
    if (Test-Path -LiteralPath $root) { $searchRoots += $root }
  }

  $packagedPlugins = @{}
  $resolvedPluginCache = @{}
  $pluginQueue = New-Object System.Collections.Queue

  function Add-PackagedPlugin {
    param(
      [Parameter(Mandatory=$true)][string]$PluginName,
      [Parameter(Mandatory=$true)][string]$SourceDir,
      [string]$HostPluginsDir,
      [System.Collections.IDictionary]$PackagedPlugins,
      [System.Collections.Queue]$PluginQueue
    )

    if ([string]::IsNullOrWhiteSpace($PluginName)) {
      throw "Cannot package a plugin with an empty name."
    }
    if ([string]::IsNullOrWhiteSpace($SourceDir)) {
      throw "Cannot package plugin '$PluginName' because its source directory was not resolved."
    }
    if ($PackagedPlugins.ContainsKey($PluginName)) {
      return
    }

    $destDir = Join-Path $HostPluginsDir $PluginName
    Write-Host "Packaging plugin '$PluginName' from '$SourceDir'"
    Copy-Item -LiteralPath $SourceDir -Destination $destDir -Recurse -Force
    $descriptorPath = Join-Path $destDir "$PluginName.uplugin"
    $PackagedPlugins[$PluginName] = @{
      Path = $destDir
      Descriptor = $descriptorPath
    }
    $PluginQueue.Enqueue($PluginName)
  }

  function Get-PluginDependencies {
    param([string]$DescriptorPath)

    if (!(Test-Path -LiteralPath $DescriptorPath)) {
      Write-Warning "Descriptor '$DescriptorPath' not found while evaluating dependencies."
      return @()
    }

    try {
      $content = Get-Content -LiteralPath $DescriptorPath -Raw | ConvertFrom-Json
    } catch {
      Write-Warning "Failed to parse descriptor '$DescriptorPath': $_"
      return @()
    }

    if (-not $content.Plugins) {
      return @()
    }

    return $content.Plugins | Where-Object {
      $_ -and $_.Enabled -ne $false -and ![string]::IsNullOrWhiteSpace($_.Name)
    } | Select-Object -ExpandProperty Name
  }

  function Find-PluginSourceDir {
    param(
      [string]$PluginName,
      [string[]]$SearchRoots,
      [System.Collections.IDictionary]$Cache
    )

    if ($Cache.ContainsKey($PluginName)) {
      return $Cache[$PluginName]
    }

    foreach ($root in $SearchRoots) {
      if (!(Test-Path -LiteralPath $root)) {
        continue
      }
      $match = Get-ChildItem -LiteralPath $root -Recurse -Filter "$PluginName.uplugin" -File -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($match) {
        $Cache[$PluginName] = $match.Directory.FullName
        return $match.Directory.FullName
      }
    }

    $Cache[$PluginName] = $null
    return $null
  }

  Add-PackagedPlugin -PluginName $pluginName -SourceDir $pluginRoot -HostPluginsDir $hostPluginsDir -PackagedPlugins $packagedPlugins -PluginQueue $pluginQueue

  foreach ($dir in $resolvedAdditionalDirs) {
    $descriptor = Get-ChildItem -LiteralPath $dir -Filter *.uplugin -File | Select-Object -First 1
    if (-not $descriptor) {
      Write-Warning "No .uplugin descriptor found under '$dir'. Skipping."
      continue
    }
    $additionalName = [IO.Path]::GetFileNameWithoutExtension($descriptor.FullName)
    Add-PackagedPlugin -PluginName $additionalName -SourceDir $descriptor.Directory.FullName -HostPluginsDir $hostPluginsDir -PackagedPlugins $packagedPlugins -PluginQueue $pluginQueue
  }

  while ($pluginQueue.Count -gt 0) {
    $current = $pluginQueue.Dequeue()
    $descriptorPath = $packagedPlugins[$current].Descriptor
    $dependencies = Get-PluginDependencies -DescriptorPath $descriptorPath
    foreach ($dep in $dependencies) {
      if ($packagedPlugins.ContainsKey($dep)) {
        continue
      }
      $source = Find-PluginSourceDir -PluginName $dep -SearchRoots $searchRoots -Cache $resolvedPluginCache
      if ($source) {
        Add-PackagedPlugin -PluginName $dep -SourceDir $source -HostPluginsDir $hostPluginsDir -PackagedPlugins $packagedPlugins -PluginQueue $pluginQueue
      } else {
        Write-Warning "Dependency plugin '$dep' could not be located. The packaged project may fail to launch if this plugin is required."
      }
    }
  }

  $pluginEntries = $packagedPlugins.Keys | Sort-Object | ForEach-Object { @{ Name = $_; Enabled = $true } }

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
  } catch {
    Write-Error "Fallback packaging failed: $($_.Exception.Message)"
    if ($_.InvocationInfo) {
      Write-Host $_.InvocationInfo.PositionMessage
    }
    throw
  }
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
