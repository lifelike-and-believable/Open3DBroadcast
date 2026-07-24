# Build Scripts and Tools

This directory contains build scripts, test runners, utilities for developing and testing Unreal Engine plugins, and CMake configurations for building prebuilt libraries.

## Directory Structure

```
Build/
└── Scripts/          # PowerShell and bash scripts for building and testing
```

## Plugins Overview

### Open3DBroadcast Plugin
Located at `ProjectSandbox/Plugins/Open3DBroadcast/`, this plugin vendors the o3ds core library's headers and compiled static library under `ThirdParty/open3dstream/`. As of the fix for issues #203/#204, that vendored tree is no longer git-committed - it's rebuilt from source on every CI run (see `Sync-O3DSCore.ps1` below) using the same root `CMakeLists.txt` recipe `.github/workflows/windows.yml` uses to produce release zips.

## Building O3DS Core From Source

#### `Sync-O3DSCore.ps1`
Builds the o3ds core library (and its NNG/CML/CRCpp/FlatBuffers dependencies) from source, then copies the compiled static library and `src/o3ds` headers/source directly into the Open3DBroadcast plugin's `ThirdParty/open3dstream/` tree. Run this before building the plugin itself - all four `open3dbroadcast-plugin-*.yml` CI workflows call it automatically.

**Usage:**
```powershell
.\Build\Scripts\Sync-O3DSCore.ps1
```

**Parameters:**
- `-RepoRoot` - Path to the repository root (default: inferred from script location)
- `-BuildDir` - Scratch directory for CMake build trees (default: `<RepoRoot>/_o3ds_build`, already covered by the repo root `.gitignore`)
- `-Configuration` - CMake build configuration (default: `Release`)
- `-PluginRoot` - Path to the Open3DBroadcast plugin root (default: `<RepoRoot>/ProjectSandbox/Plugins/Open3DBroadcast`)

**Note**: Headers are copied directly from `src/o3ds`, not from the CMake install tree - `src/CMakeLists.txt`'s `PUBLIC_HEADER` install rule does not actually install any headers (verified empirically). Only the compiled library and the flatc-generated `o3ds_generated.h` come from the CMake install output.

## Scripts

### Setup and Configuration

#### `Setup-UE.ps1`
Verifies Unreal Engine installation.

**Usage:**
```powershell
.\Build\Scripts\Setup-UE.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7"
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine installation (default: `C:\Program Files\Epic Games\UE_5.7`)

---

### Building

#### `Build-Plugin.ps1`
Builds an Unreal plugin using Unreal Automation Tool (UAT).

**Usage:**
```powershell
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutDir "Artifacts\Open3DBroadcast" `
  -TargetPlatforms @("Win64") `
  -Configuration "Shipping"
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine installation (required)
- `-PluginUPluginPath` - Path to `.uplugin` file (required)
- `-OutDir` - Output directory for packaged plugin (required)
- `-TargetPlatforms` - Array of platforms to build (default: `@("Win64")`)
- `-Configuration` - Build configuration: Development, Shipping, etc. (default: `Development`)

**Output:**
- Packaged plugin in `OutDir`
- Ready to install in other Unreal projects

---

### Testing

#### `Run-AutomationTests.ps1`
Runs Unreal's automation tests for the plugin.

**Usage:**
```powershell
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DBroadcast.*" `
  -ResultsDir "Artifacts\Tests"
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine (required)
- `-ProjectFile` - Path to `.uproject` file (required)
- `-TestFilter` - Test filter pattern (default: `"*"`)
- `-ResultsDir` - Output directory for test results (default: `"Artifacts\Tests"`)

**Output:**
- `Results.xml` - Test results in XML format
- Console output with test status

#### `Run-Gauntlet.ps1`
Runs Gauntlet integration tests.

**Usage:**
```powershell
.\Build\Scripts\Run-Gauntlet.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "ProjectSandbox\ProjectSandbox.uproject" `
  -GauntletConfigs @("Open3DBroadcastTests") `
  -OutputDir "Artifacts\Gauntlet" `
  -NullRHI
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine (required)
- `-ProjectFile` - Path to `.uproject` file (required)
- `-GauntletConfigs` - Array of Gauntlet config names (required)
- `-OutputDir` - Output directory for reports (default: `"Artifacts\Gauntlet"`)
- `-NullRHI` - Switch to use NullRHI (headless mode)

**Output:**
- `Index.html` - Main report page
- `TestReport.json` - Machine-readable results
- Logs and screenshots

---

## Common Workflows

### Initial Setup

```powershell
# 1. Verify UE installation
.\Build\Scripts\Setup-UE.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7"

# 2. The plugin lives in-tree at ProjectSandbox/Plugins/Open3DBroadcast - nothing to link
```

### Local Development

```powershell
# Build plugin for local testing
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "$PWD\ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutDir "$PWD\Artifacts\Local"
```

### Running Tests

```powershell
# Run all plugin automation tests
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DBroadcast.*"

# Run Gauntlet integration tests
.\Build\Scripts\Run-Gauntlet.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -GauntletConfigs @("Open3DBroadcastTests") `
  -NullRHI
```

### Full Build and Test

```powershell
# Complete workflow
.\Build\Scripts\Setup-UE.ps1
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "$PWD\ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutDir "$PWD\Artifacts\Win64"
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DBroadcast.*"
```

## CI/CD Integration

These scripts are used by the GitHub Actions workflows to build the
Open3DBroadcast plugin:

- **open3dbroadcast-plugin-ci.yml** - Builds plugin for CI validation
- **open3dbroadcast-plugin-test.yml** - Runs the automation test suite
- **open3dbroadcast-plugin-nightly.yml** - Nightly plugin builds
- **open3dbroadcast-plugin-release.yml** - Release builds with Shipping configuration

All four call `Sync-O3DSCore.ps1` first, then package the plugin for
distribution.

See `.github/workflows/` for workflow definitions.

### Launch Unreal Editor on a self-hosted runner

We provide a convenience workflow to open the Unreal Editor on a self-hosted machine—useful for local validation runs.

- Workflow: `dev-open-ue-editor` (`.github/workflows/open-ue-editor.yml`)
- Trigger: Manual (workflow_dispatch)
- Inputs:
  - `ue_path` (required): Absolute UE root (e.g., `C:\\Program Files\\Epic Games\\UE_5.7` or `/opt/Unreal/UE_5.7`)
  - `project` (optional): Path to `.uproject` (default: `ProjectSandbox/ProjectSandbox.uproject`)
  - `map` (optional): Map to load (e.g., `/Game/Maps/Example`)
  - `extra_args` (optional): Editor CLI args (default: `-log`)
  - `detach` (optional): Start detached (default: `true`)

Requirements:
- A self-hosted runner on the target machine with Unreal installed and a user session capable of launching GUI apps.
- Windows and Linux are supported; macOS can be added similarly.

Usage:
1. In GitHub → Actions → `dev-open-ue-editor`, click “Run workflow”.
2. Fill in `ue_path` and optionally override `project`, `map`, `extra_args`, or `detach`.
3. Select the appropriate self-hosted runner and run.


## Platform Support

| Script | Windows | Linux/Mac |
|--------|---------|-----------|
| Setup-UE | ✅ | ❌ |
| Build-Plugin | ✅ | ❌ |
| Run-AutomationTests | ✅ | ❌ |
| Run-Gauntlet | ✅ | ❌ |

**Note:** Linux/Mac support can be added by creating bash equivalents of the PowerShell scripts.

## Requirements

- **Windows**: PowerShell 5.1+ (or PowerShell Core 7+)
- **Unreal Engine**: 5.4 or later
- **Visual Studio**: 2022 (for building)
- **Git**: For repository operations

## Troubleshooting

### "RunUAT not found"
- Verify UE installation path
- Ensure UE includes Engine/Build directory
- Run Setup-UE.ps1 first

### "Plugin file not found"
- Check plugin path is correct
- Ensure you're running from repository root
- Verify `ProjectSandbox/Plugins/Open3DBroadcast/Open3DBroadcast.uplugin` exists

### Test failures
- Check test logs in ResultsDir
- Ensure plugin is properly linked
- Verify project opens in UE Editor without errors

## Contributing

When adding new scripts:
1. Follow existing naming conventions
2. Add comprehensive parameter help
3. Include error handling
4. Update this README
5. Test on clean environment

## Resources

- [Unreal Automation Tool (UAT)](https://docs.unrealengine.com/5.4/en-US/unreal-automation-tool-in-unreal-engine/)
- [BuildPlugin Command](https://docs.unrealengine.com/5.4/en-US/using-the-buildplugin-command-in-unreal-engine/)
- [Automation Testing](https://docs.unrealengine.com/5.4/en-US/automation-system-overview-in-unreal-engine/)
- [Gauntlet Framework](https://docs.unrealengine.com/5.4/en-US/gauntlet-automation-framework-in-unreal-engine/)
