# Build Scripts and Tools

This directory contains build scripts, test runners, and utilities for developing and testing the Open3DStream Unreal Engine plugin.

## Directory Structure

```
Build/
└── Scripts/          # PowerShell and bash scripts for building and testing
```

## Scripts

### Setup and Configuration

#### `Setup-UE.ps1`
Verifies Unreal Engine installation.

**Usage:**
```powershell
.\Build\Scripts\Setup-UE.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.4"
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine installation (default: `C:\Program Files\Epic Games\UE_5.4`)

---

### Plugin Linking

#### `Link-PluginIntoSandbox.ps1` (Windows)
Creates junction links from both plugins to ProjectSandbox.

**Usage:**
```powershell
.\Build\Scripts\Link-PluginIntoSandbox.ps1
```

**What it does:**
- Creates `ProjectSandbox/Plugins/Open3DStream` → `plugins/unreal/Open3DStream`
- Creates `ProjectSandbox/Plugins/Open3DBroadcast` → `plugins/unreal/Open3DBroadcast`
- Removes existing links if present
- Automatically determines repository paths

#### `link_plugin_into_sandbox.sh` (Linux/Mac)
Creates a symbolic link from the plugin to ProjectSandbox.

**Usage:**
```bash
./Build/Scripts/link_plugin_into_sandbox.sh
```

---

### Building

#### `Build-Plugin.ps1`
Builds the plugin using Unreal Automation Tool (UAT).

**Usage:**
```powershell
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -PluginUPluginPath "plugins\unreal\Open3DStream\Open3DStream.uplugin" `
  -OutDir "Artifacts\Win64" `
  -TargetPlatforms @("Win64") `
  -Configuration "Development"
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
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DStream.*" `
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
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "ProjectSandbox\ProjectSandbox.uproject" `
  -GauntletConfigs @("Open3DStreamTests") `
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
.\Build\Scripts\Setup-UE.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.4"

# 2. Link plugin into sandbox
.\Build\Scripts\Link-PluginIntoSandbox.ps1
```

### Local Development

```powershell
# Build plugin for local testing
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -PluginUPluginPath "$PWD\plugins\unreal\Open3DStream\Open3DStream.uplugin" `
  -OutDir "$PWD\Artifacts\Local"
```

### Running Tests

```powershell
# Run all plugin automation tests
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DStream.*"

# Run Gauntlet integration tests
.\Build\Scripts\Run-Gauntlet.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -GauntletConfigs @("Open3DStreamTests") `
  -NullRHI
```

### Full Build and Test

```powershell
# Complete workflow
.\Build\Scripts\Setup-UE.ps1
.\Build\Scripts\Link-PluginIntoSandbox.ps1
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -PluginUPluginPath "$PWD\plugins\unreal\Open3DStream\Open3DStream.uplugin" `
  -OutDir "$PWD\Artifacts\Win64"
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DStream.*"
```

## CI/CD Integration

These scripts are used by the GitHub Actions workflows to build both Open3DStream and Open3DBroadcast plugins:

- **unreal-plugin-ci.yml** - Builds both plugins for CI validation
- **unreal-plugin-agent-ci.yml** - Same as CI, triggered by agent workflows
- **unreal-plugin-nightly.yml** - Nightly builds of both plugins
- **unreal-plugin-release.yml** - Release builds of both plugins with Shipping configuration

All workflows package both plugins together into a unified artifact for easy distribution.

See `.github/workflows/` for workflow definitions.

## Platform Support

| Script | Windows | Linux/Mac |
|--------|---------|-----------|
| Setup-UE | ✅ | ❌ |
| Link-PluginIntoSandbox | ✅ (`.ps1`) | ✅ (`.sh`) |
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
- Verify `plugins/unreal/Open3DStream/Open3DStream.uplugin` exists

### "Link-PluginIntoSandbox fails"
- Run PowerShell as Administrator (for junctions on Windows)
- Check that ProjectSandbox directory exists
- Remove existing Plugins directory if it's not a junction

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
