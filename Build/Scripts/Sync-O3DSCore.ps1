<#
.SYNOPSIS
    Builds the O3DS core library from source and syncs it into the
    Open3DBroadcast plugin's vendored ThirdParty/open3dstream tree.

.DESCRIPTION
    Replaces the previous manual workflow (rebuild open3dstreamstatic.lib
    locally, hand-copy src/o3ds headers, git add the result) with a scripted
    build + sync so the compiled library is never committed to the repo.

    Builds NNG/CML/CRCpp/FlatBuffers to a scratch install prefix, then
    configures and builds the repo's root CMakeLists.txt (the same recipe
    .github/workflows/windows.yml already uses to produce release zips),
    and copies the result into the plugin tree:
      - the compiled static library -> ThirdParty/open3dstream/lib/Win64
      - src/o3ds (headers + source, verbatim) -> ThirdParty/open3dstream/include/o3ds
      - src/o3ds.fbs -> ThirdParty/open3dstream/include/o3ds.fbs
      - the flatc-generated o3ds_generated.h -> ThirdParty/open3dstream/include

    Headers/source are copied directly from src/o3ds rather than from the
    CMake install tree: src/CMakeLists.txt's
    `install(TARGETS ... PUBLIC_HEADER DESTINATION include/o3ds ...)` rule
    does not actually install any headers (verified empirically - CMake does
    not preserve the o3ds/ subdirectory structure this target's PUBLIC_HEADER
    list relies on), so `cmake --install` alone would silently produce a
    header-less vendored tree. The generated flatbuffers header is installed
    fine (it's a plain install(FILES ...) rule, not PUBLIC_HEADER), so that
    one *is* taken from the install tree.

.PARAMETER RepoRoot
    Path to the repository root. Defaults to two levels up from this script
    (Build/Scripts/Sync-O3DSCore.ps1 -> repo root).

.PARAMETER BuildDir
    Scratch directory for CMake build trees and the dependency install
    prefix. Defaults to $RepoRoot/_o3ds_core_build.

.PARAMETER Configuration
    CMake build configuration. Defaults to Release.

.PARAMETER PluginRoot
    Path to the Open3DBroadcast plugin root (the directory containing
    ThirdParty/). Defaults to $RepoRoot/ProjectSandbox/Plugins/Open3DBroadcast.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path,
    [string]$BuildDir = (Join-Path $RepoRoot "_o3ds_core_build"),
    [string]$Configuration = "Release",
    [string]$PluginRoot = (Join-Path $RepoRoot "ProjectSandbox\Plugins\Open3DBroadcast")
)

$ErrorActionPreference = "Stop"

function Invoke-Checked
{
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$CmdArgs
    )
    & $Exe @CmdArgs
    if ($LASTEXITCODE -ne 0)
    {
        throw "$Exe $($CmdArgs -join ' ') failed with exit code $LASTEXITCODE"
    }
}

$Prefix = Join-Path $BuildDir "usr"
$InstallDir = Join-Path $BuildDir "out"
New-Item -ItemType Directory -Force -Path $Prefix | Out-Null

Write-Host "==> Building o3ds dependencies (NNG, CML, CRCpp, FlatBuffers) into $Prefix"
$Deps = @("thirdparty/nng", "thirdparty/cml", "thirdparty/crccpp", "thirdparty/flatbuffers")
foreach ($Dep in $Deps)
{
    $DepPath = Join-Path $RepoRoot $Dep
    $DepBuild = Join-Path $BuildDir ("dep_" + (Split-Path $Dep -Leaf))
    Write-Host "  -- $Dep"
    Invoke-Checked "cmake" @(
        "-S", $DepPath, "-B", $DepBuild,
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_INSTALL_PREFIX=$Prefix",
        "-DCMAKE_PREFIX_PATH=$Prefix"
    )
    Invoke-Checked "cmake" @("--build", $DepBuild, "--config", $Configuration)
    Invoke-Checked "cmake" @("--install", $DepBuild, "--config", $Configuration)
}

Write-Host "==> Configuring + building o3ds core"
$O3DSBuild = Join-Path $BuildDir "o3ds"
Invoke-Checked "cmake" @(
    "-S", $RepoRoot, "-B", $O3DSBuild,
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_PREFIX_PATH=$Prefix",
    "-DCMAKE_INSTALL_PREFIX=$InstallDir",
    "-DO3DS_DISABLE_WEBRTC=ON",
    "-DO3DS_BUILD_TESTS=OFF"
)
Invoke-Checked "cmake" @("--build", $O3DSBuild, "--config", $Configuration)
Invoke-Checked "cmake" @("--install", $O3DSBuild, "--config", $Configuration)

# --- Sync compiled library ---
$LibOutDir = Join-Path $PluginRoot "ThirdParty\open3dstream\lib\Win64"
New-Item -ItemType Directory -Force -Path $LibOutDir | Out-Null
$BuiltLib = Get-ChildItem -Path $InstallDir -Recurse -Filter "open3dstreamstatic*.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $BuiltLib)
{
    $BuiltLib = Get-ChildItem -Path $InstallDir -Recurse -Filter "libopen3dstreamstatic*.a" -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (-not $BuiltLib)
{
    throw "Could not find built open3dstreamstatic library under $InstallDir"
}
$LibDest = Join-Path $LibOutDir "open3dstreamstatic.lib"
Copy-Item -Path $BuiltLib.FullName -Destination $LibDest -Force
Write-Host "==> Copied $($BuiltLib.FullName) -> $LibDest"

# --- Sync headers/source directly from src/o3ds (see doc comment above) ---
$IncludeOutDir = Join-Path $PluginRoot "ThirdParty\open3dstream\include"
New-Item -ItemType Directory -Force -Path $IncludeOutDir | Out-Null

$SrcO3DS = Join-Path $RepoRoot "src\o3ds"
$DstO3DS = Join-Path $IncludeOutDir "o3ds"
if (Test-Path $DstO3DS)
{
    Remove-Item -Path $DstO3DS -Recurse -Force
}
Copy-Item -Path $SrcO3DS -Destination $DstO3DS -Recurse -Force
Write-Host "==> Synced $SrcO3DS -> $DstO3DS"

Copy-Item -Path (Join-Path $RepoRoot "src\o3ds.fbs") -Destination (Join-Path $IncludeOutDir "o3ds.fbs") -Force

$GeneratedHeader = Get-ChildItem -Path $InstallDir -Recurse -Filter "o3ds_generated.h" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $GeneratedHeader)
{
    throw "Could not find generated o3ds_generated.h under $InstallDir - was FlatBuffers codegen run?"
}
Copy-Item -Path $GeneratedHeader.FullName -Destination (Join-Path $IncludeOutDir "o3ds_generated.h") -Force
Write-Host "==> Copied $($GeneratedHeader.FullName) -> $IncludeOutDir\o3ds_generated.h"

Write-Host "==> o3ds core sync complete."
