param(
  [Parameter(Mandatory = $true)][string]$PluginSourcePath,   # e.g. D:\ue-ci\workspaces\pr-123\plugins\unreal\Open3DStream
  [Parameter(Mandatory = $true)][string]$HostUProjectPath,   # e.g. E:\UnrealProjects\Open3DStreamProject\Open3DStreamProject.uproject
  [Parameter(Mandatory = $true)][string]$PluginFolderName    # e.g. Open3DStream
)

$hostDir    = Split-Path -Path $HostUProjectPath -Parent
$dstPlugins = Join-Path $hostDir "Plugins"
$dst        = Join-Path $dstPlugins $PluginFolderName

Write-Host "Host dir: $hostDir"
Write-Host "Ensuring Plugins dir exists at $dstPlugins"
New-Item -ItemType Directory -Force -Path $dstPlugins | Out-Null

if (Test-Path $dst) {
  Write-Host "Removing existing plugin folder or junction at $dst"
  # Try to remove as junction first to avoid recursing into source by mistake
  try { cmd /c rmdir "$dst" 2>$null | Out-Null } catch { }
  if (Test-Path $dst) { Remove-Item -Recurse -Force -Path $dst }
}

Write-Host "Creating junction: $dst -> $PluginSourcePath"

# Prefer PowerShell Junction (PS 5+). Fall back to mklink /J if needed.
try {
  New-Item -ItemType Junction -Path $dst -Target $PluginSourcePath | Out-Null
} catch {
  Write-Host "New-Item Junction failed, falling back to mklink /J..."
  $cmd = "mklink /J `"$dst`" `"$PluginSourcePath`""
  cmd /c $cmd | Write-Host
  if (-not (Test-Path $dst)) { throw "Failed to create junction with both methods." }
}

Write-Host "Junction created successfully."
