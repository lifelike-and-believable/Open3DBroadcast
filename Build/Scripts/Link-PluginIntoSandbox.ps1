$ScriptRoot = Split-Path -Parent $PSScriptRoot
$RepoRoot = Split-Path -Parent $ScriptRoot

$SandboxPath = Join-Path $RepoRoot "ProjectSandbox"
$dstParent = Join-Path $SandboxPath "Plugins"
New-Item -ItemType Directory -Force -Path $dstParent | Out-Null

# Link Open3DStream plugin (which now includes Open3DBroadcast as a module)
$plugins = @("Open3DStream")

foreach ($pluginName in $plugins) {
  $PluginPath = Join-Path $RepoRoot "plugins\unreal\$pluginName"
  $dst = Join-Path $SandboxPath "Plugins\$pluginName"

  if (!(Test-Path -LiteralPath $PluginPath)) {
    Write-Warning "Plugin not found at: $PluginPath (skipping)"
    continue
  }

  if (Test-Path $dst) { 
    Write-Host "Removing existing link: $dst"
    Remove-Item $dst -Recurse -Force 
  }

  Write-Host "Creating junction for $pluginName..."
  Write-Host "  From: $PluginPath"
  Write-Host "  To:   $dst"

  cmd /c "mklink /J ""$dst"" ""$PluginPath"""

  if (Test-Path $dst) {
    Write-Host "Successfully linked $pluginName into sandbox: $dst"
  } else {
    Write-Error "Failed to create junction for $pluginName"
    exit 1
  }
}
