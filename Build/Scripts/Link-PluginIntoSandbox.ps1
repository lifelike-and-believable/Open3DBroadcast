$ScriptRoot = Split-Path -Parent $PSScriptRoot
$RepoRoot = Split-Path -Parent $ScriptRoot

$PluginPath = Join-Path $RepoRoot "plugins\unreal\Open3DStream"
$SandboxPath = Join-Path $RepoRoot "ProjectSandbox"
$dst = Join-Path $SandboxPath "Plugins\Open3DStream"

if (!(Test-Path -LiteralPath $PluginPath)) {
  Write-Error "Plugin not found at: $PluginPath"
  exit 1
}

if (Test-Path $dst) { 
  Write-Host "Removing existing link: $dst"
  Remove-Item $dst -Recurse -Force 
}

$dstParent = Split-Path $dst
New-Item -ItemType Directory -Force -Path $dstParent | Out-Null

Write-Host "Creating junction..."
Write-Host "  From: $PluginPath"
Write-Host "  To:   $dst"

cmd /c "mklink /J ""$dst"" ""$PluginPath"""

if (Test-Path $dst) {
  Write-Host "✓ Successfully linked plugin into sandbox: $dst"
} else {
  Write-Error "Failed to create junction"
  exit 1
}
